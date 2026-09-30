// Merge sort of an index vector under a caller-supplied total preorder.
// Correctness: the output is sorted and is the same bag as the input
// (every sum over it is unchanged).
use vstd::prelude::*;
use crate::sums::*;

verus! {

// A list of items is sorted if, for every pair where one comes earlier, the earlier item is
// 'less-or-equal' to the later one under the given comparison; the correctness goal of sorting.
pub open spec fn sorted_le(s: Seq<usize>, le: spec_fn(usize, usize) -> bool) -> bool {
    forall|i: int, j: int| 0 <= i < j < s.len() ==> le(#[trigger] s[i], #[trigger] s[j])
}

// Every item in the list is a legitimate one, i.e. within the set on which the comparison is
// well behaved (total and transitive); sorting only promises anything for such items.
pub open spec fn all_valid(s: Seq<usize>, valid: spec_fn(usize) -> bool) -> bool {
    forall|i: int| 0 <= i < s.len() ==> valid(#[trigger] s[i])
}

/// le is a total preorder on valid elements.
pub open spec fn preorder_on(le: spec_fn(usize, usize) -> bool, valid: spec_fn(usize) -> bool) -> bool {
    &&& forall|a: usize, b: usize| #![trigger le(a, b)] valid(a) && valid(b) ==> le(a, b) || le(b, a)
    &&& forall|a: usize, b: usize, c: usize|
        valid(a) && valid(b) && valid(c) && #[trigger] le(a, b) && #[trigger] le(b, c) ==> le(a, c)
}

// The real comparison function is safe to call on legitimate items and its answer always
// agrees with the abstract less-or-equal relation; links running code to the sort proof.
pub open spec fn cmp_ok<F: Fn(usize, usize) -> bool>(
    cmp: &F,
    le: spec_fn(usize, usize) -> bool,
    valid: spec_fn(usize) -> bool,
) -> bool {
    &&& forall|a: usize, b: usize| valid(a) && valid(b) ==> #[trigger] cmp.requires((a, b))
    &&& forall|a: usize, b: usize, r: bool| #[trigger] cmp.ensures((a, b), r) ==> r == le(a, b)
}

// Two lists contain the same items with the same multiplicities (one is a reordering of the other),
// shown by every weighted sum over them agreeing; guarantees sorting neither loses nor invents rows.
pub open spec fn same_bag(a: Seq<usize>, b: Seq<usize>) -> bool {
    forall|f: spec_fn(usize) -> int| #[trigger] sum(a, f) == sum(b, f)
}

// Merge two already-sorted lists into one sorted list by repeatedly taking the smaller head.
// It also tracks that the output plus what remains has the same items as the inputs.
fn merge<F: Fn(usize, usize) -> bool>(
    l: &Vec<usize>,
    r: &Vec<usize>,
    cmp: &F,
    Ghost(le): Ghost<spec_fn(usize, usize) -> bool>,
    Ghost(valid): Ghost<spec_fn(usize) -> bool>,
) -> (out: Vec<usize>)
    requires
        cmp_ok(cmp, le, valid),
        preorder_on(le, valid),
        all_valid(l@, valid),
        all_valid(r@, valid),
        sorted_le(l@, le),
        sorted_le(r@, le),
    ensures
        all_valid(out@, valid),
        sorted_le(out@, le),
        forall|f: spec_fn(usize) -> int| #[trigger] sum(out@, f) == sum(l@, f) + sum(r@, f),
{
    // Room for both inputs up front (capacity is only an allocation hint).
    let cap = if l.len() <= usize::MAX - r.len() {
        l.len() + r.len()
    } else {
        l.len()
    };
    let mut out: Vec<usize> = Vec::with_capacity(cap);
    let mut i: usize = 0;
    let mut j: usize = 0;
    while i < l.len() || j < r.len()
        invariant
            cmp_ok(cmp, le, valid),
            preorder_on(le, valid),
            all_valid(l@, valid),
            all_valid(r@, valid),
            sorted_le(l@, le),
            sorted_le(r@, le),
            0 <= i <= l@.len(),
            0 <= j <= r@.len(),
            all_valid(out@, valid),
            sorted_le(out@, le),
            forall|k: int| 0 <= k < out@.len() && i < l@.len() ==> le(#[trigger] out@[k], l@[i as int]),
            forall|k: int| 0 <= k < out@.len() && j < r@.len() ==> le(#[trigger] out@[k], r@[j as int]),
            forall|f: spec_fn(usize) -> int|
                #[trigger] sum(out@, f) == sum(l@.take(i as int), f) + sum(r@.take(j as int), f),
        decreases l@.len() - i + r@.len() - j,
    {
        let take_left = if i < l.len() && j < r.len() {
            cmp(l[i], r[j])
        } else {
            i < l.len()
        };
        let ghost o0 = out@;
        if take_left {
            let x = l[i];
            out.push(x);
            // Took the head of the left list: the output's sums grow by that item, it stays sorted, and it
            // is still below both remaining heads, since the left head was less-or-equal to the right one.
            proof {
                assert forall|f: spec_fn(usize) -> int| #[trigger] sum(out@, f) == sum(
                    l@.take(i as int + 1),
                    f,
                ) + sum(r@.take(j as int), f) by {
                    lemma_sum_push(o0, x, f);
                    lemma_sum_take(l@, i as int, f);
                }
                assert(sorted_le(out@, le)) by {
                    assert forall|a: int, b: int| 0 <= a < b < out@.len() implies le(
                        #[trigger] out@[a],
                        #[trigger] out@[b],
                    ) by {
                        if b < o0.len() {
                        } else {
                            assert(out@[a] == o0[a]);
                        }
                    }
                }
                if i + 1 < l@.len() {
                    assert(le(l@[i as int], l@[i as int + 1]));
                    assert forall|k: int| 0 <= k < out@.len() implies le(
                        #[trigger] out@[k],
                        l@[i as int + 1],
                    ) by {
                        if k < o0.len() {
                            assert(le(o0[k], l@[i as int]));
                        }
                    }
                }
                if j < r@.len() {
                    if i < l@.len() && j < r@.len() {
                    }
                    assert forall|k: int| 0 <= k < out@.len() implies le(
                        #[trigger] out@[k],
                        r@[j as int],
                    ) by {
                        if k < o0.len() {
                        } else {
                            assert(valid(x) && valid(r@[j as int]));
                        }
                    }
                }
            }
            i += 1;
        } else {
            let x = r[j];
            out.push(x);
            // Took the head of the right list because the left head was not less-or-equal to it; by totality
            // the right item is then below the left head, so the output stays sorted and item-preserving.
            proof {
                assert forall|f: spec_fn(usize) -> int| #[trigger] sum(out@, f) == sum(
                    l@.take(i as int),
                    f,
                ) + sum(r@.take(j as int + 1), f) by {
                    lemma_sum_push(o0, x, f);
                    lemma_sum_take(r@, j as int, f);
                }
                if i < l@.len() {
                    // cmp said not le(l[i], r[j]), so le(r[j], l[i]) by totality
                    assert(valid(l@[i as int]) && valid(x));
                    assert(le(x, l@[i as int]));
                }
                assert(sorted_le(out@, le)) by {
                    assert forall|a: int, b: int| 0 <= a < b < out@.len() implies le(
                        #[trigger] out@[a],
                        #[trigger] out@[b],
                    ) by {
                        if b < o0.len() {
                        } else {
                            assert(out@[a] == o0[a]);
                        }
                    }
                }
                if j + 1 < r@.len() {
                    assert(le(r@[j as int], r@[j as int + 1]));
                    assert forall|k: int| 0 <= k < out@.len() implies le(
                        #[trigger] out@[k],
                        r@[j as int + 1],
                    ) by {
                        if k < o0.len() {
                            assert(le(o0[k], r@[j as int]));
                        }
                    }
                }
                if i < l@.len() {
                    assert forall|k: int| 0 <= k < out@.len() implies le(
                        #[trigger] out@[k],
                        l@[i as int],
                    ) by {
                        if k < o0.len() {
                        }
                    }
                }
            }
            j += 1;
        }
    }
    // Both lists are fully consumed, so the loop's running item-count facts become the final guarantee.
    proof {
        assert(l@.take(l@.len() as int) =~= l@);
        assert(r@.take(r@.len() as int) =~= r@);
    }
    out
}

/// Sorts v[lo..hi].
pub fn msort<F: Fn(usize, usize) -> bool>(
    v: &Vec<usize>,
    lo: usize,
    hi: usize,
    cmp: &F,
    Ghost(le): Ghost<spec_fn(usize, usize) -> bool>,
    Ghost(valid): Ghost<spec_fn(usize) -> bool>,
) -> (out: Vec<usize>)
    requires
        lo <= hi <= v@.len(),
        cmp_ok(cmp, le, valid),
        preorder_on(le, valid),
        all_valid(v@, valid),
    ensures
        all_valid(out@, valid),
        sorted_le(out@, le),
        forall|f: spec_fn(usize) -> int| #[trigger]
            sum(out@, f) == sum(v@.subrange(lo as int, hi as int), f),
    decreases hi - lo,
{
    if hi - lo <= 1 {
        let mut out: Vec<usize> = Vec::new();
        if hi > lo {
            out.push(v[lo]);
            // Range of length 1: the single output item is exactly the one-element slice.
            proof {
                assert(out@ =~= v@.subrange(lo as int, hi as int));
            }
        } else {
            // Empty range: the empty output is exactly the empty slice.
            proof {
                assert(out@ =~= v@.subrange(lo as int, hi as int));
            }
        }
        return out;
    }
    let mid = lo + (hi - lo) / 2;
    let l = msort(v, lo, mid, cmp, Ghost(le), Ghost(valid));
    let r = msort(v, mid, hi, cmp, Ghost(le), Ghost(valid));
    let out = merge(&l, &r, cmp, Ghost(le), Ghost(valid));
    // Merge sort combine step: the slice is the left half followed by the right half, and sums add over
    // concatenation, so the merged result contains exactly the same items as the slice.
    proof {
        assert forall|f: spec_fn(usize) -> int| #[trigger] sum(out@, f) == sum(
            v@.subrange(lo as int, hi as int),
            f,
        ) by {
            assert(v@.subrange(lo as int, hi as int) =~= v@.subrange(lo as int, mid as int)
                + v@.subrange(mid as int, hi as int));
            lemma_sum_concat(
                v@.subrange(lo as int, mid as int),
                v@.subrange(mid as int, hi as int),
                f,
            );
        }
    }
    out
}

// Sort a list of row indices by the given comparison: the result is sorted, contains only legitimate
// items and is a reordering of the input. Used to ORDER BY the query result.
pub fn sort_idx<F: Fn(usize, usize) -> bool>(
    v: &Vec<usize>,
    cmp: &F,
    Ghost(le): Ghost<spec_fn(usize, usize) -> bool>,
    Ghost(valid): Ghost<spec_fn(usize) -> bool>,
) -> (out: Vec<usize>)
    requires
        cmp_ok(cmp, le, valid),
        preorder_on(le, valid),
        all_valid(v@, valid),
    ensures
        all_valid(out@, valid),
        sorted_le(out@, le),
        same_bag(out@, v@),
{
    let out = msort(v, 0, v.len(), cmp, Ghost(le), Ghost(valid));
    // The slice covering the whole list is the list itself, so the result is a reordering of the input.
    proof {
        assert(v@.subrange(0, v@.len() as int) =~= v@);
    }
    out
}

} // verus!

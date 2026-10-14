// Executable SQL LIKE, verified against the frozen spec verus_common::sqlfns::like_match.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::sqlfns::*;

verus! {

/// like_match of the suffixes s[i..], p[j..].
pub open spec fn like_suffix(s: Seq<u8>, p: Seq<u8>, i: int, j: int) -> bool {
    like_match(s.skip(i), p.skip(j))
}

/// Unfolding of like_match on suffixes (0 <= i <= |s|, 0 <= j <= |p|).
pub proof fn lemma_like_suffix_step(s: Seq<u8>, p: Seq<u8>, i: int, j: int)
    requires 0 <= i <= s.len(), 0 <= j <= p.len(),
    ensures
        j == p.len() ==> like_suffix(s, p, i, j) == (i == s.len()),
        j < p.len() && p[j] == 37u8 ==> like_suffix(s, p, i, j)
            == (like_suffix(s, p, i, j + 1) || (i < s.len() && like_suffix(s, p, i + 1, j))),
        j < p.len() && p[j] != 37u8 ==> like_suffix(s, p, i, j)
            == (i < s.len() && (p[j] == 95u8 || p[j] == s[i]) && like_suffix(s, p, i + 1, j + 1)),
{
    let ss = s.skip(i);
    let ps = p.skip(j);
    reveal(like_match);
    if j < p.len() {
        assert(ps[0] == p[j]);
        assert(ps.drop_first() =~= p.skip(j + 1));
    }
    if i < s.len() {
        assert(ss[0] == s[i]);
        assert(ss.drop_first() =~= s.skip(i + 1));
    }
}

/// From like_suffix(a, b) at a '%' (p[b] == '%'): some k >= a matches the rest after the '%'.
proof fn lemma_like_star_down(s: Seq<u8>, p: Seq<u8>, a: int, b: int)
    requires 0 <= a <= s.len(), 0 <= b < p.len(), p[b] == 37u8, like_suffix(s, p, a, b),
    ensures exists|k: int| a <= k <= s.len() && #[trigger] like_suffix(s, p, k, b + 1),
    decreases s.len() - a,
{
    lemma_like_suffix_step(s, p, a, b);
    if !like_suffix(s, p, a, b + 1) {
        lemma_like_star_down(s, p, a + 1, b);
    }
}

/// A '%'-free pattern segment p[c..c+len) matched at k consumes exactly len bytes.
proof fn lemma_like_seg(s: Seq<u8>, p: Seq<u8>, k: int, c: int, len: int)
    requires
        0 <= k <= s.len(), 0 <= c, 0 <= len, c + len <= p.len(),
        forall|t: int| c <= t < c + len ==> #[trigger] p[t] != 37u8,
        like_suffix(s, p, k, c),
    ensures k + len <= s.len(), like_suffix(s, p, k + len, c + len),
    decreases len,
{
    if len > 0 {
        lemma_like_suffix_step(s, p, k, c);
        lemma_like_seg(s, p, k + 1, c + 1, len - 1);
    }
}

/// At a '%' pattern byte, a match of a later suffix gives a match of any earlier suffix.
proof fn lemma_like_pct_back(s: Seq<u8>, p: Seq<u8>, i: int, a: int, j: int)
    requires 0 <= i <= a <= s.len(), 0 <= j < p.len(), p[j] == 37u8, like_suffix(s, p, a, j),
    ensures like_suffix(s, p, i, j),
    decreases a - i,
{
    if a > i {
        lemma_like_suffix_step(s, p, a - 1, j);
        lemma_like_pct_back(s, p, i, a - 1, j);
    }
}

/// Greedy-matching core: after '%' at b, a '%'-free segment p[b+1..j), then matching the rest
/// from a later suffix a leaves a witness k >= a that has consumed the segment.
proof fn lemma_like_star_seg(s: Seq<u8>, p: Seq<u8>, a: int, b: int, j: int)
    requires
        0 <= a <= s.len(), 0 <= b < j <= p.len(), p[b] == 37u8,
        forall|t: int| b < t < j ==> #[trigger] p[t] != 37u8,
        like_suffix(s, p, a, b),
    ensures exists|k: int| a <= k && k + (j - b - 1) <= s.len() && #[trigger] like_suffix(s, p, k + (j - b - 1), j),
{
    lemma_like_star_down(s, p, a, b);
    let k = choose|k: int| a <= k <= s.len() && #[trigger] like_suffix(s, p, k, b + 1);
    lemma_like_seg(s, p, k, b + 1, j - b - 1);
}

/// Use to run a runtime SQL LIKE (pattern given at run time): equals `like_match`.
/// Greedy two-pointer matching with backtracking to the last '%' (O(|s|+|p|) on typical
/// patterns, O(|s|*|p|) worst case), no buffer.
pub fn like_match_exec(s: &[u8], p: &[u8]) -> (r: bool)
    ensures r == like_match(s@, p@),
{
    let n = s.len();
    let m = p.len();
    proof { assert(s@.skip(0) =~= s@); assert(p@.skip(0) =~= p@); }
    let mut i: usize = 0;
    let mut j: usize = 0;
    let mut star = false;
    let mut si: usize = 0;
    let mut sj: usize = 0;
    // !star: like(i, j) == like(0, 0).  star: p[sj-1] is the last '%', the segment p[sj..j)
    // has matched s[si..i), like(0, 0) == like(si, sj-1) and like(si, sj) == like(i, j).
    while i < n
        invariant
            n == s@.len(), m == p@.len(), i <= n, j <= m,
            !star ==> like_suffix(s@, p@, i as int, j as int) == like_match(s@, p@),
            star ==> (
                1 <= sj <= j && si <= i && (i - si) == (j - sj) && p@[sj - 1] == 37u8
                && (forall|t: int| sj <= t < j ==> #[trigger] p@[t] != 37u8)
                && like_match(s@, p@) == like_suffix(s@, p@, si as int, sj as int - 1)
                && like_suffix(s@, p@, si as int, sj as int) == like_suffix(s@, p@, i as int, j as int)),
        decreases (if star { n - si } else { n + 1 }), m - j,
    {
        if j < m && p[j] == 37u8 {
            proof {
                if star {
                    lemma_like_suffix_step(s@, p@, si as int, sj as int - 1);
                    if si < n && like_suffix(s@, p@, si as int + 1, sj as int - 1) {
                        lemma_like_star_seg(s@, p@, si as int + 1, sj as int - 1, j as int);
                        let k = choose|k: int| si as int + 1 <= k
                            && k + (j as int - (sj as int - 1) - 1) <= n
                            && #[trigger] like_suffix(s@, p@, k + (j as int - (sj as int - 1) - 1), j as int);
                        lemma_like_pct_back(s@, p@, i as int, k + (j as int - sj as int), j as int);
                    }
                    lemma_like_suffix_step(s@, p@, si as int, sj as int - 1);
                }
            }
            star = true;
            si = i;
            sj = j + 1;
            j = j + 1;
        } else if j < m && (p[j] == 95u8 || p[j] == s[i]) {
            proof { lemma_like_suffix_step(s@, p@, i as int, j as int); }
            i += 1;
            j += 1;
        } else {
            proof { lemma_like_suffix_step(s@, p@, i as int, j as int); }
            if star {
                proof { lemma_like_suffix_step(s@, p@, si as int, sj as int - 1); }
                si += 1;
                i = si;
                j = sj;
            } else {
                return false;
            }
        }
    }
    let mut jj = j;
    while jj < m && p[jj] == 37u8
        invariant
            j <= jj <= m, m == p@.len(), n == s@.len(), i == n,
            like_suffix(s@, p@, n as int, j as int) == like_suffix(s@, p@, n as int, jj as int),
        decreases m - jj,
    {
        proof { lemma_like_suffix_step(s@, p@, n as int, jj as int); }
        jj += 1;
    }
    proof {
        lemma_like_suffix_step(s@, p@, n as int, jj as int);
        if star {
            lemma_like_suffix_step(s@, p@, si as int, sj as int - 1);
            if si < n && like_suffix(s@, p@, si as int + 1, sj as int - 1) {
                lemma_like_star_seg(s@, p@, si as int + 1, sj as int - 1, j as int);
            }
        }
    }
    jj == m
}

} // verus!

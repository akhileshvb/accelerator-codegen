// Facts about the specification itself (verified, not trusted): q21_result
// (and q21_full_result, the answer before LIMIT) determines exactly one
// output for every database and nation.
//
//  * lemma_q21_unique: two outputs satisfying q21_full_result are equal.
//  * lemma_q21_exists: some output satisfies q21_full_result for every db and
//    nation, so the postcondition is not vacuous (execute may still return
//    None on overflow; this says a correct answer always exists).
//  * q21_answer names that unique answer.
//  * lemma_q21_limited_iff_answer: q21_result holds exactly for the first
//    q21_limit() rows of q21_answer, so execute's postcondition pins down
//    "the" answer, not "an" answer.
use vstd::prelude::*;
use verus_common::bytes::*;
use verus_common::sums::*;
use crate::spec::*;
use crate::bridge::*;

verus! {

// ── row_before is a strict total order on rows with distinct names ─────────
proof fn lemma_rb_irrefl(x: (Seq<u8>, int))
    ensures
        !row_before(x, x),
{
    lemma_bytes_lt_irrefl(x.0);
}

proof fn lemma_rb_asym(x: (Seq<u8>, int), y: (Seq<u8>, int))
    ensures
        !(row_before(x, y) && row_before(y, x)),
{
    lemma_bytes_lt_asym(x.0, y.0);
}

proof fn lemma_rb_trans(x: (Seq<u8>, int), y: (Seq<u8>, int), z: (Seq<u8>, int))
    requires
        row_before(x, y),
        row_before(y, z),
    ensures
        row_before(x, z),
{
    if x.1 == y.1 && y.1 == z.1 {
        lemma_bytes_lt_trans(x.0, y.0, z.0);
    }
}

proof fn lemma_rb_total(x: (Seq<u8>, int), y: (Seq<u8>, int))
    requires
        x.0 != y.0,
    ensures
        row_before(x, y) || row_before(y, x),
{
    lemma_bytes_lt_total(x.0, y.0);
}

/// Strictly increasing in ORDER BY order (the first conjunct of q21_full_result).
pub open spec fn sorted(s: Seq<(Seq<u8>, int)>) -> bool {
    forall|i: int, j: int| 0 <= i < j < s.len() ==> row_before(#[trigger] s[i], #[trigger] s[j])
}

pub open spec fn has_name(s: Seq<(Seq<u8>, int)>, g: Seq<u8>) -> bool {
    exists|k: int| 0 <= k < s.len() && (#[trigger] s[k]).0 == g
}

// ── Uniqueness ─────────────────────────────────────────────────────────────
/// Two strictly sorted sequences with the same elements are equal.
proof fn lemma_sorted_same_elems(a: Seq<(Seq<u8>, int)>, b: Seq<(Seq<u8>, int)>)
    requires
        sorted(a),
        sorted(b),
        forall|x: (Seq<u8>, int)| a.contains(x) <==> b.contains(x),
    ensures
        a == b,
    decreases a.len() + b.len(),
{
    if a.len() == 0 {
        if b.len() > 0 {
            assert(b.contains(b[0]));
        }
        assert(a =~= b);
    } else if b.len() == 0 {
        assert(a.contains(a[0]));
    } else {
        // the heads agree: each head is in the other sequence, and a later
        // position would contradict asymmetry / irreflexivity
        assert(a.contains(a[0]));
        let j = choose|j: int| 0 <= j < b.len() && b[j] == a[0];
        assert(b.contains(b[0]));
        let i = choose|i: int| 0 <= i < a.len() && a[i] == b[0];
        if j > 0 {
            assert(row_before(b[0], b[j]));
            if i > 0 {
                assert(row_before(a[0], a[i]));
                lemma_rb_asym(a[0], b[0]);
            } else {
                lemma_rb_irrefl(b[0]);
            }
            assert(false);
        }
        assert(a[0] == b[0]);
        let a1 = a.drop_first();
        let b1 = b.drop_first();
        assert forall|x: (Seq<u8>, int)| a1.contains(x) implies b1.contains(x) by {
            let k = choose|k: int| 0 <= k < a1.len() && a1[k] == x;
            assert(a[k + 1] == x);
            assert(a.contains(x));
            let m = choose|m: int| 0 <= m < b.len() && b[m] == x;
            assert(row_before(a[0], a[k + 1]));
            if m == 0 {
                lemma_rb_irrefl(x);
            }
            assert(b1[m - 1] == x);
        }
        assert forall|x: (Seq<u8>, int)| b1.contains(x) implies a1.contains(x) by {
            let k = choose|k: int| 0 <= k < b1.len() && b1[k] == x;
            assert(b[k + 1] == x);
            assert(b.contains(x));
            let m = choose|m: int| 0 <= m < a.len() && a[m] == x;
            assert(row_before(b[0], b[k + 1]));
            if m == 0 {
                lemma_rb_irrefl(x);
            }
            assert(a1[m - 1] == x);
        }
        assert(sorted(a1)) by {
            assert forall|i: int, j: int| 0 <= i < j < a1.len() implies row_before(#[trigger] a1[i], #[trigger] a1[j]) by {
                assert(a1[i] == a[i + 1] && a1[j] == a[j + 1]);
            }
        }
        assert(sorted(b1)) by {
            assert forall|i: int, j: int| 0 <= i < j < b1.len() implies row_before(#[trigger] b1[i], #[trigger] b1[j]) by {
                assert(b1[i] == b[i + 1] && b1[j] == b[j + 1]);
            }
        }
        lemma_sorted_same_elems(a1, b1);
        assert(a =~= seq![a[0]] + a1);
        assert(b =~= seq![b[0]] + b1);
    }
}

/// Every row of one answer is a row of the other.
proof fn lemma_q21_rows_in(db: Db, p: Seq<u8>, a: Seq<(Seq<u8>, int)>, b: Seq<(Seq<u8>, int)>)
    requires
        q21_full_result(db, p, a),
        q21_full_result(db, p, b),
    ensures
        forall|x: (Seq<u8>, int)| a.contains(x) ==> b.contains(x),
{
    assert forall|x: (Seq<u8>, int)| a.contains(x) implies b.contains(x) by {
        let i = choose|i: int| 0 <= i < a.len() && a[i] == x;
        assert(a[i].1 == numwait(db, p, a[i].0) && a[i].1 > 0);
        assert(numwait(db, p, x.0) > 0);
        let k = choose|k: int| 0 <= k < b.len() && (#[trigger] b[k]).0 == x.0;
        assert(b[k].1 == numwait(db, p, b[k].0));
        assert(b[k] == x);
    }
}

/// q21_full_result has at most one solution.
pub proof fn lemma_q21_unique(db: Db, p: Seq<u8>, a: Seq<(Seq<u8>, int)>, b: Seq<(Seq<u8>, int)>)
    requires
        q21_full_result(db, p, a),
        q21_full_result(db, p, b),
    ensures
        a == b,
{
    lemma_q21_rows_in(db, p, a, b);
    lemma_q21_rows_in(db, p, b, a);
    lemma_sorted_same_elems(a, b);
}

// ── Existence ──────────────────────────────────────────────────────────────
/// Insert x into a sorted sequence at its ORDER BY position.
pub open spec fn insert(s: Seq<(Seq<u8>, int)>, x: (Seq<u8>, int)) -> Seq<(Seq<u8>, int)>
    decreases s.len(),
{
    if s.len() == 0 {
        seq![x]
    } else if row_before(x, s[0]) {
        seq![x] + s
    } else {
        seq![s[0]] + insert(s.drop_first(), x)
    }
}

proof fn lemma_insert(s: Seq<(Seq<u8>, int)>, x: (Seq<u8>, int))
    requires
        sorted(s),
        !has_name(s, x.0),
    ensures
        sorted(insert(s, x)),
        forall|y: (Seq<u8>, int)|
            #![trigger insert(s, x).contains(y)]
            #![trigger s.contains(y)]
            insert(s, x).contains(y) <==> (s.contains(y) || y == x),
    decreases s.len(),
{
    let r = insert(s, x);
    if s.len() == 0 {
        assert forall|y: (Seq<u8>, int)| r.contains(y) <==> (s.contains(y) || y == x) by {
            if r.contains(y) {
                let k = choose|k: int| 0 <= k < r.len() && r[k] == y;
                assert(k == 0);
            }
            if y == x {
                assert(r[0] == y);
            }
        }
    } else if row_before(x, s[0]) {
        assert(r == seq![x] + s);
        assert forall|i: int, j: int| 0 <= i < j < r.len() implies row_before(#[trigger] r[i], #[trigger] r[j]) by {
            if i == 0 {
                assert(r[j] == s[j - 1]);
                if j > 1 {
                    assert(row_before(s[0], s[j - 1]));
                    lemma_rb_trans(x, s[0], s[j - 1]);
                }
            } else {
                assert(r[i] == s[i - 1] && r[j] == s[j - 1]);
            }
        }
        assert forall|y: (Seq<u8>, int)| r.contains(y) <==> (s.contains(y) || y == x) by {
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
                assert(r[0] == y);
            }
        }
    } else {
        let s1 = s.drop_first();
        assert(sorted(s1)) by {
            assert forall|i: int, j: int| 0 <= i < j < s1.len() implies row_before(#[trigger] s1[i], #[trigger] s1[j]) by {
                assert(s1[i] == s[i + 1] && s1[j] == s[j + 1]);
            }
        }
        assert(!has_name(s1, x.0)) by {
            if has_name(s1, x.0) {
                let k = choose|k: int| 0 <= k < s1.len() && (#[trigger] s1[k]).0 == x.0;
                assert(s[k + 1].0 == x.0);
            }
        }
        lemma_insert(s1, x);
        let t = insert(s1, x);
        assert(r == seq![s[0]] + t);
        assert(s[0].0 != x.0);
        lemma_rb_total(s[0], x);
        assert(row_before(s[0], x));
        assert forall|i: int, j: int| 0 <= i < j < r.len() implies row_before(#[trigger] r[i], #[trigger] r[j]) by {
            if i == 0 {
                assert(r[j] == t[j - 1]);
                assert(t.contains(t[j - 1]));
                if t[j - 1] != x {
                    let k = choose|k: int| 0 <= k < s1.len() && s1[k] == t[j - 1];
                    assert(s[k + 1] == t[j - 1]);
                }
            } else {
                assert(r[i] == t[i - 1] && r[j] == t[j - 1]);
            }
        }
        assert forall|y: (Seq<u8>, int)| r.contains(y) <==> (s.contains(y) || y == x) by {
            if r.contains(y) {
                let k = choose|k: int| 0 <= k < r.len() && r[k] == y;
                if k > 0 {
                    assert(t[k - 1] == y);
                    assert(t.contains(y));
                    if s1.contains(y) {
                        let m = choose|m: int| 0 <= m < s1.len() && s1[m] == y;
                        assert(s[m + 1] == y);
                    }
                } else {
                    assert(s[0] == y);
                }
            }
            if s.contains(y) {
                let k = choose|k: int| 0 <= k < s.len() && s[k] == y;
                if k > 0 {
                    assert(s1[k - 1] == y);
                    assert(s1.contains(y));
                    let m = choose|m: int| 0 <= m < t.len() && t[m] == y;
                    assert(r[m + 1] == y);
                } else {
                    assert(r[0] == y);
                }
            }
            if y == x {
                assert(t.contains(x));
                let m = choose|m: int| 0 <= m < t.len() && t[m] == x;
                assert(r[m + 1] == y);
            }
        }
    }
}

/// Fold the suppliers' names into a sorted answer: add each name with a
/// positive numwait the first time it is seen.
pub open spec fn build_answer(db: Db, p: Seq<u8>, sups: Seq<Supplier>) -> Seq<(Seq<u8>, int)>
    decreases sups.len(),
{
    if sups.len() == 0 {
        Seq::empty()
    } else {
        let r = build_answer(db, p, sups.drop_last());
        let g = sups.last().name;
        if numwait(db, p, g) > 0 && !has_name(r, g) {
            insert(r, (g, numwait(db, p, g)))
        } else {
            r
        }
    }
}

proof fn lemma_build_answer(db: Db, p: Seq<u8>, sups: Seq<Supplier>)
    ensures
        sorted(build_answer(db, p, sups)),
        forall|i: int|
            0 <= i < build_answer(db, p, sups).len() ==> (#[trigger] build_answer(db, p, sups)[i]).1
                == numwait(db, p, build_answer(db, p, sups)[i].0) && build_answer(db, p, sups)[i].1 > 0,
        forall|k: int|
            0 <= k < sups.len() && numwait(db, p, (#[trigger] sups[k]).name) > 0 ==> has_name(
                build_answer(db, p, sups),
                sups[k].name,
            ),
    decreases sups.len(),
{
    if sups.len() > 0 {
        let s0 = sups.drop_last();
        lemma_build_answer(db, p, s0);
        let r = build_answer(db, p, s0);
        let g = sups.last().name;
        let res = build_answer(db, p, sups);
        if numwait(db, p, g) > 0 && !has_name(r, g) {
            let x = (g, numwait(db, p, g));
            lemma_insert(r, x);
            assert(res == insert(r, x));
            assert forall|i: int| 0 <= i < res.len() implies (#[trigger] res[i]).1 == numwait(db, p, res[i].0)
                && res[i].1 > 0 by {
                assert(res.contains(res[i]));
                if res[i] != x {
                    let m = choose|m: int| 0 <= m < r.len() && r[m] == res[i];
                    assert(r[m].1 == numwait(db, p, r[m].0));
                }
            }
            assert forall|k: int| 0 <= k < sups.len() && numwait(db, p, (#[trigger] sups[k]).name) > 0 implies has_name(
                res,
                sups[k].name,
            ) by {
                if k < sups.len() - 1 {
                    assert(s0[k] == sups[k]);
                    let m = choose|m: int| 0 <= m < r.len() && (#[trigger] r[m]).0 == sups[k].name;
                    assert(r.contains(r[m]));
                    assert(res.contains(r[m]));
                    let n = choose|n: int| 0 <= n < res.len() && res[n] == r[m];
                    assert(res[n].0 == sups[k].name);
                } else {
                    assert(res.contains(x));
                    let n = choose|n: int| 0 <= n < res.len() && res[n] == x;
                    assert(res[n].0 == sups[k].name);
                }
            }
        } else {
            assert(res == r);
            assert forall|k: int| 0 <= k < sups.len() && numwait(db, p, (#[trigger] sups[k]).name) > 0 implies has_name(
                res,
                sups[k].name,
            ) by {
                if k < sups.len() - 1 {
                    assert(s0[k] == sups[k]);
                }
            }
        }
    }
}

/// A group with a positive count is the name of some supplier row.
proof fn lemma_group_has_supplier(db: Db, p: Seq<u8>, g: Seq<u8>) -> (k: int)
    requires
        numwait(db, p, g) > 0,
    ensures
        0 <= k < db.supplier.len(),
        db.supplier[k].name == g,
{
    lemma_numwait_by_supplier(db, p, g);
    lemma_sum_nonzero_witness(db.supplier, |s: Supplier| sup_term(db, p, g, s))
}

/// q21_full_result has a solution for every database and nation.
pub proof fn lemma_q21_exists(db: Db, p: Seq<u8>)
    ensures
        q21_full_result(db, p, build_answer(db, p, db.supplier)),
{
    let out = build_answer(db, p, db.supplier);
    lemma_build_answer(db, p, db.supplier);
    assert forall|g: Seq<u8>| #[trigger] numwait(db, p, g) > 0 implies exists|i: int|
        0 <= i < out.len() && (#[trigger] out[i]).0 == g by {
        let k = lemma_group_has_supplier(db, p, g);
        assert(numwait(db, p, db.supplier[k].name) > 0);
        assert(has_name(out, db.supplier[k].name));
    }
}

/// The answer to Q21 on db for :nation = p.
pub open spec fn q21_answer(db: Db, p: Seq<u8>) -> Seq<(Seq<u8>, int)> {
    build_answer(db, p, db.supplier)
}

/// Satisfying q21_full_result is the same as being the answer.
pub proof fn lemma_q21_result_iff_answer(db: Db, p: Seq<u8>, out: Seq<(Seq<u8>, int)>)
    ensures
        q21_full_result(db, p, out) <==> out == q21_answer(db, p),
{
    lemma_q21_exists(db, p);
    if q21_full_result(db, p, out) {
        lemma_q21_unique(db, p, out, q21_answer(db, p));
    }
}

/// Satisfying q21_result (with LIMIT) is the same as being the first
/// q21_limit() rows of the answer.
pub proof fn lemma_q21_limited_iff_answer(db: Db, p: Seq<u8>, out: Seq<(Seq<u8>, int)>)
    ensures
        q21_result(db, p, out) <==> out == limit_rows(q21_answer(db, p), q21_limit()),
{
    lemma_q21_exists(db, p);
    if q21_result(db, p, out) {
        let full = choose|full: Seq<(Seq<u8>, int)>|
            #[trigger] q21_full_result(db, p, full) && out == limit_rows(full, q21_limit());
        lemma_q21_unique(db, p, full, q21_answer(db, p));
    }
}

} // verus!

// Every set of answer rows of distinct groups has a sorted listing.
#![allow(unused_imports)]
use vstd::prelude::*;
use crate::gen_spec::*;
use crate::ordering::*;

verus! {

pub open spec fn rows_sorted(t: Seq<OutRow>) -> bool {
    forall|i: int, j: int| 0 <= i < j < t.len() ==> !row_lt(#[trigger] t[j], #[trigger] t[i])
}

pub open spec fn rows_distinct(t: Seq<OutRow>) -> bool {
    forall|i: int, j: int| 0 <= i < j < t.len() ==> !same_group(#[trigger] t[i], #[trigger] t[j])
}

pub open spec fn all_crow(t: Seq<OutRow>) -> bool {
    forall|i: int| 0 <= i < t.len() ==> crow(#[trigger] t[i])
}

proof fn lemma_first_row(rows: Seq<OutRow>) -> (m: int)
    requires rows.len() > 0, all_crow(rows),
    ensures 0 <= m < rows.len(), forall|i: int| 0 <= i < rows.len() ==> !row_lt(#[trigger] rows[i], rows[m]),
    decreases rows.len(),
{
    let t = rows.drop_last();
    if t.len() == 0 {
        assert forall|i: int| 0 <= i < rows.len() implies !row_lt(#[trigger] rows[i], rows[0]) by {
            lemma_row_lt_order(rows[0], rows[0], rows[0]);
        }
        0
    } else {
        assert(all_crow(t)) by { assert forall|i: int| 0 <= i < t.len() implies crow(#[trigger] t[i]) by { assert(t[i] == rows[i]); } }
        let m0 = lemma_first_row(t);
        let last = rows[rows.len() - 1];
        if row_lt(last, rows[m0]) {
            assert forall|i: int| 0 <= i < rows.len() implies !row_lt(#[trigger] rows[i], last) by {
                if i < t.len() {
                    assert(rows[i] == t[i]);
                    lemma_row_lt_order(rows[i], last, rows[m0]);
                } else {
                    lemma_row_lt_order(last, last, last);
                }
            }
            rows.len() - 1
        } else {
            assert forall|i: int| 0 <= i < rows.len() implies !row_lt(#[trigger] rows[i], rows[m0]) by {
                if i < t.len() { assert(rows[i] == t[i]); }
            }
            m0
        }
    }
}

pub proof fn lemma_sort_rows(rows: Seq<OutRow>) -> (t: Seq<OutRow>)
    requires all_crow(rows), rows_distinct(rows),
    ensures
        t.len() == rows.len(),
        forall|i: int| 0 <= i < t.len() ==> rows.contains(#[trigger] t[i]),
        forall|i: int| 0 <= i < rows.len() ==> t.contains(#[trigger] rows[i]),
        rows_sorted(t), rows_distinct(t),
    decreases rows.len(),
{
    if rows.len() == 0 {
        Seq::empty()
    } else {
        let m = lemma_first_row(rows);
        let rest = rows.remove(m);
        assert forall|i: int| 0 <= i < rest.len() implies #[trigger] rest[i] == if i < m { rows[i] } else { rows[i + 1] } by {}
        assert(all_crow(rest));
        assert(rows_distinct(rest)) by {
            assert forall|i: int, j: int| 0 <= i < j < rest.len() implies !same_group(#[trigger] rest[i], #[trigger] rest[j]) by {
                let (a, b) = (if i < m { i } else { i + 1 }, if j < m { j } else { j + 1 });
                assert(a < b);
                assert(rest[i] == rows[a] && rest[j] == rows[b]);
            }
        }
        let t0 = lemma_sort_rows(rest);
        let t = seq![rows[m]] + t0;
        assert forall|i: int| 0 <= i < t.len() implies rows.contains(#[trigger] t[i]) by {
            if i > 0 {
                let x = t0[i - 1];
                assert(rest.contains(x));
                let q = choose|q: int| 0 <= q < rest.len() && rest[q] == x;
                assert(rows[if q < m { q } else { q + 1 }] == x);
            } else {
                assert(t[0] == rows[m]);
            }
        }
        assert forall|i: int| 0 <= i < rows.len() implies t.contains(#[trigger] rows[i]) by {
            if i == m {
                assert(t[0] == rows[m]);
            } else {
                let q = if i < m { i } else { i - 1 };
                assert(rest[q] == rows[i]);
                assert(t0.contains(rest[q]));
                let z = choose|z: int| 0 <= z < t0.len() && t0[z] == rest[q];
                assert(t[z + 1] == rows[i]);
            }
        }
        assert(rows_sorted(t)) by {
            assert forall|i: int, j: int| 0 <= i < j < t.len() implies !row_lt(#[trigger] t[j], #[trigger] t[i]) by {
                if i == 0 {
                    assert(t[j] == t0[j - 1]);
                    assert(rows.contains(t0[j - 1]));
                    let q = choose|q: int| 0 <= q < rows.len() && rows[q] == t0[j - 1];
                } else {
                    assert(t[i] == t0[i - 1] && t[j] == t0[j - 1]);
                }
            }
        }
        assert(rows_distinct(t)) by {
            assert forall|i: int, j: int| 0 <= i < j < t.len() implies !same_group(#[trigger] t[i], #[trigger] t[j]) by {
                if i == 0 {
                    let x = t0[j - 1];
                    assert(rest.contains(x));
                    let q = choose|q: int| 0 <= q < rest.len() && rest[q] == x;
                    let a = if q < m { q } else { q + 1 };
                    assert(rows[a] == x && a != m);
                    if a < m { assert(!same_group(rows[a], rows[m])); } else { assert(!same_group(rows[m], rows[a])); }
                } else {
                    assert(t[i] == t0[i - 1] && t[j] == t0[j - 1]);
                }
            }
        }
        t
    }
}

} // verus!

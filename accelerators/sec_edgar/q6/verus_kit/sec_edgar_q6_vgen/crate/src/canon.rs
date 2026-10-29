// A key list that is sorted and without repetition is determined by its members.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::bytes::*;

verus! {

pub open spec fn strictly_sorted(ks: Seq<Seq<u8>>) -> bool {
    forall|i: int, j: int| 0 <= i < j < ks.len() ==> bytes_lt(#[trigger] ks[i], #[trigger] ks[j])
}

/// ks holds exactly the keys pred holds for, once each, in order.
pub open spec fn canon_keys(ks: Seq<Seq<u8>>, pred: spec_fn(Seq<u8>) -> bool) -> bool {
    &&& strictly_sorted(ks)
    &&& forall|a: Seq<u8>| #![trigger ks.contains(a)] ks.contains(a) <==> pred(a)
}

pub proof fn lemma_canon_unique(a: Seq<Seq<u8>>, b: Seq<Seq<u8>>, pred: spec_fn(Seq<u8>) -> bool)
    requires canon_keys(a, pred), canon_keys(b, pred),
    ensures a == b,
    decreases a.len() + b.len(),
{
    if a.len() == 0 && b.len() == 0 {
        assert(a =~= b);
    } else if a.len() == 0 {
        assert(b.contains(b[0])); assert(pred(b[0])); assert(a.contains(b[0]));
    } else if b.len() == 0 {
        assert(a.contains(a[0])); assert(pred(a[0])); assert(b.contains(a[0]));
    } else {
        // the first keys agree
        assert(a.contains(a[0])); assert(b.contains(b[0]));
        assert(pred(a[0]) && pred(b[0]));
        assert(b.contains(a[0])); assert(a.contains(b[0]));
        let i = choose|i: int| 0 <= i < b.len() && b[i] == a[0];
        let j = choose|j: int| 0 <= j < a.len() && a[j] == b[0];
        if i > 0 { assert(bytes_lt(b[0], b[i])); }
        if j > 0 { assert(bytes_lt(a[0], a[j])); }
        if i > 0 && j > 0 {
            lemma_bytes_lt_asym(b[0], a[0]);
            lemma_bytes_lt_asym(a[0], b[0]);
        }
        if i > 0 { assert(j == 0 || true); }
        assert(a[0] == b[0]) by {
            if i > 0 { assert(bytes_lt(b[0], a[0])); if j > 0 { assert(bytes_lt(a[0], b[0])); lemma_bytes_lt_asym(a[0], b[0]); } else { assert(a[j] == b[0]); lemma_bytes_lt_irrefl(a[0]); } }
            if j > 0 && i == 0 { assert(bytes_lt(a[0], b[0])); assert(b[0] == a[0]); lemma_bytes_lt_irrefl(a[0]); }
        }
        let ta = a.drop_first();
        let tb = b.drop_first();
        assert(strictly_sorted(ta) && strictly_sorted(tb));
        let pred2 = |x: Seq<u8>| pred(x) && x != a[0];
        assert forall|x: Seq<u8>| ta.contains(x) <==> pred2(x) by {
            if ta.contains(x) {
                let q = choose|q: int| 0 <= q < ta.len() && ta[q] == x;
                assert(a[q + 1] == x);
                assert(a.contains(x));
                assert(bytes_lt(a[0], x));
                lemma_bytes_lt_irrefl(a[0]);
            }
            if pred2(x) {
                assert(a.contains(x));
                let q = choose|q: int| 0 <= q < a.len() && a[q] == x;
                if q == 0 { assert(false); }
                assert(ta[q - 1] == x);
            }
        }
        assert forall|x: Seq<u8>| tb.contains(x) <==> pred2(x) by {
            if tb.contains(x) {
                let q = choose|q: int| 0 <= q < tb.len() && tb[q] == x;
                assert(b[q + 1] == x);
                assert(b.contains(x));
                assert(bytes_lt(b[0], x));
                lemma_bytes_lt_irrefl(b[0]);
            }
            if pred2(x) {
                assert(b.contains(x));
                let q = choose|q: int| 0 <= q < b.len() && b[q] == x;
                if q == 0 { assert(false); }
                assert(tb[q - 1] == x);
            }
        }
        lemma_canon_unique(ta, tb, pred2);
        assert(a =~= seq![a[0]] + ta);
        assert(b =~= seq![b[0]] + tb);
    }
}

} // verus!

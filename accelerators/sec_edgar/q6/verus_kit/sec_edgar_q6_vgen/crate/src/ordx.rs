// The stored group key sorts like the ORDER BY columns; rank keys order the answer rows.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::bytes::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{KeyView, key_lt, lemma_key_order};
use crate::gen_spec::*;
use crate::model::*;
use crate::esc::*;
use crate::ordering::*;
use crate::f64bits::*;
use crate::grp::*;

verus! {

proof fn lemma_lt_prefix(p: Seq<u8>, x: Seq<u8>, y: Seq<u8>)
    ensures bytes_lt(p + x, p + y) == bytes_lt(x, y),
    decreases p.len(),
{
    if p.len() == 0 {
        assert(p + x =~= x);
        assert(p + y =~= y);
    } else {
        assert((p + x).drop_first() =~= p.drop_first() + x);
        assert((p + y).drop_first() =~= p.drop_first() + y);
        lemma_lt_prefix(p.drop_first(), x, y);
    }
}

/// The escaped encoding keeps the byte order of the strings, whatever follows.
proof fn lemma_esc_lt(a: Seq<u8>, b: Seq<u8>, r1: Seq<u8>, r2: Seq<u8>)
    requires bytes_lt(a, b),
    ensures bytes_lt(esc(a) + r1, esc(b) + r2),
    decreases a.len(),
{
    if a.len() == 0 {
        lemma_body_head(b);
        assert(esc(a) =~= seq![0u8]);
        assert((esc(a) + r1)[0] == 0u8);
        assert((esc(b) + r2)[0] == 1u8);
        reveal_with_fuel(bytes_lt, 2);
    } else {
        lemma_body_head(a);
        lemma_body_head(b);
        let (ha, hb) = (seq![1u8, a[0]], seq![1u8, b[0]]);
        assert(esc(a) + r1 =~= ha + (esc(a.skip(1)) + r1));
        assert(esc(b) + r2 =~= hb + (esc(b.skip(1)) + r2));
        reveal_with_fuel(bytes_lt, 3);
        if a[0] == b[0] {
            assert(a.drop_first() =~= a.skip(1));
            assert(b.drop_first() =~= b.skip(1));
            lemma_esc_lt(a.skip(1), b.skip(1), r1, r2);
            lemma_lt_prefix(ha, esc(a.skip(1)) + r1, esc(b.skip(1)) + r2);
        }
    }
}

proof fn lemma_esc_opt_lt(x: Option<Seq<u8>>, y: Option<Seq<u8>>, r1: Seq<u8>, r2: Seq<u8>)
    requires opt_lt(x, y),
    ensures bytes_lt(esc_opt(x) + r1, esc_opt(y) + r2),
{
    if y is None {
        let a = x->0;
        if a.len() > 0 { lemma_body_head(a); }
        assert(esc(a)[0] == 0u8 || esc(a)[0] == 1u8);
        assert((esc_opt(x) + r1)[0] == esc(a)[0]);
        assert((esc_opt(y) + r2)[0] == 2u8);
    } else {
        lemma_esc_lt(x->0, y->0, r1, r2);
    }
}

/// ORDER BY name, tag, plabel on group keys.
pub open spec fn lexi_lt(n1: Option<Seq<u8>>, t1: Seq<u8>, p1: Option<Seq<u8>>, n2: Option<Seq<u8>>, t2: Seq<u8>, p2: Option<Seq<u8>>) -> bool {
    opt_lt(n1, n2) || (n1 == n2 && (bytes_lt(t1, t2) || (t1 == t2 && opt_lt(p1, p2))))
}

/// The stored group key sorts like the ORDER BY columns.
pub proof fn lemma_ikey_lt(n1: Option<Seq<u8>>, t1: Seq<u8>, p1: Option<Seq<u8>>, n2: Option<Seq<u8>>, t2: Seq<u8>, p2: Option<Seq<u8>>)
    requires lexi_lt(n1, t1, p1, n2, t2, p2),
    ensures bytes_lt(ikey(n1, t1, p1).1, ikey(n2, t2, p2).1),
{
    let (r1, r2) = (esc(t1) + esc_opt(p1), esc(t2) + esc_opt(p2));
    assert(ikey(n1, t1, p1).1 =~= esc_opt(n1) + r1);
    assert(ikey(n2, t2, p2).1 =~= esc_opt(n2) + r2);
    if opt_lt(n1, n2) {
        lemma_esc_opt_lt(n1, n2, r1, r2);
    } else if bytes_lt(t1, t2) {
        lemma_lt_prefix(esc_opt(n1), r1, r2);
        lemma_esc_lt(t1, t2, esc_opt(p1), esc_opt(p2));
    } else {
        let e = esc_opt(n1) + esc(t1);
        assert(ikey(n1, t1, p1).1 =~= e + esc_opt(p1));
        assert(ikey(n2, t2, p2).1 =~= e + esc_opt(p2));
        lemma_esc_opt_lt(p1, p2, Seq::empty(), Seq::empty());
        assert(esc_opt(p1) + Seq::<u8>::empty() =~= esc_opt(p1));
        assert(esc_opt(p2) + Seq::<u8>::empty() =~= esc_opt(p2));
        lemma_lt_prefix(e, esc_opt(p1), esc_opt(p2));
    }
}


/// The answer row of a stored double and group key.
pub open spec fn brow(p2: Seq<u8>, b: u64, k: GK, cnt: int) -> OutRow {
    OutRow { name: k.name, stmt: Some(p2), tag: Some(k.tag), plabel: k.plabel, total_value: Some(f64_val(b)), cnt }
}

/// A smaller rank key is an earlier answer row.
pub proof fn lemma_rk_row(p2: Seq<u8>, b1: u64, k1: GK, c1: int, b2: u64, k2: GK, c2: int)
    requires
        f64_finite(b1), f64_finite(b2),
        key_lt(rkey_gk(b1, k1), rkey_gk(b2, k2)),
    ensures
        !row_lt(brow(p2, b2, k2, c2), brow(p2, b1, k1, c1)),
{
    let (r1, r2) = (brow(p2, b1, k1, c1), brow(p2, b2, k2, c2));
    lemma_desc_key(b1, b2);
    lemma_desc_key(b2, b1);
    if row_lt(r2, r1) {
        assert(f64_val(b1) == f64_val(b2));
        assert(f64_desc_key(b1) == f64_desc_key(b2));
        assert(bytes_lt(ikey(k1.name, k1.tag, k1.plabel).1, ikey(k2.name, k2.tag, k2.plabel).1));
        lemma_ikey_lt(k2.name, k2.tag, k2.plabel, k1.name, k1.tag, k1.plabel);
        lemma_bytes_lt_asym(ikey(k1.name, k1.tag, k1.plabel).1, ikey(k2.name, k2.tag, k2.plabel).1);
    }
}

/// A row with a total below the double range does not sort before any double's row.
pub proof fn lemma_beyond_row(p2: Seq<u8>, b: u64, k: GK, c: int, r2: OutRow)
    requires f64_finite(b), r2.total_value is Some, r2.total_value->0 < -max_val(),
    ensures !row_lt(r2, brow(p2, b, k, c)),
{
    lemma_f64_in_range(b);
}

} // verus!

// The sub table bucketed by filing, each row with a signed multiplicity: the buckets stand for a table
// by the sums they give every weight (`wbag_as`), so a change is one signed row and no row need be found
// in the table before it is removed.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bag::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::Wide;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::keys::*;
use crate::rows::*;

verus! {

pub struct WRow {
    pub row: SubRt,
    pub m: Wide,
}

pub open spec fn wsum(v: Seq<WRow>, f: spec_fn(SubRow) -> int) -> int {
    sum(v, |w: WRow| w.m@ * f(sub_rt_view(w.row)))
}

pub open spec fn wrows_at(b: Index<Vec<WRow>>, k: KeyView) -> Seq<WRow> {
    match b.at(k) {
        Some(v) => v@,
        None => Seq::empty(),
    }
}

pub open spec fn skey(s: SubRow) -> KeyView {
    key1(s.adsh)
}

pub open spec fn skw(k: KeyView, f: spec_fn(SubRow) -> int) -> spec_fn(SubRow) -> int {
    |s: SubRow| if skey(s) == k { f(s) } else { 0 }
}

pub type Phi = spec_fn(spec_fn(SubRow) -> int) -> int;

/// The buckets stand for the table whose sum of any weight is phi of it.
pub open spec fn wbag_as(b: Index<Vec<WRow>>, phi: Phi) -> bool {
    &&& b.wf()
    &&& forall|k: KeyView, f: spec_fn(SubRow) -> int| #[trigger] wsum(wrows_at(b, k), f) == phi(skw(k, f))
}

pub open spec fn phi_tab(tab: Seq<SubRow>) -> Phi {
    |g: spec_fn(SubRow) -> int| sum(tab, g)
}

/// The table with one more row (sign 1) or one fewer (sign -1).
pub open spec fn phi_add(phi: Phi, row: SubRow, sg: int) -> Phi {
    |g: spec_fn(SubRow) -> int| phi(g) + sg * g(row)
}

pub fn wbag_new() -> (b: Index<Vec<WRow>>)
    ensures wbag_as(b, phi_tab(Seq::empty())),
{
    let b: Index<Vec<WRow>> = Index::new();
    proof {
        assert forall|k: KeyView, f: spec_fn(SubRow) -> int| #[trigger] wsum(wrows_at(b, k), f) == phi_tab(Seq::empty())(skw(k, f)) by {
            assert(wrows_at(b, k) =~= Seq::<WRow>::empty());
        }
    }
    b
}

fn dup_wrows(v: &Vec<WRow>) -> (out: Vec<WRow>)
    ensures
        out.len() == v.len(),
        forall|i: int| 0 <= i < v.len() ==> (#[trigger] out[i]).m@ == v[i].m@ && sub_rt_view(out[i].row) == sub_rt_view(v[i].row),
{
    let mut out: Vec<WRow> = Vec::new();
    let mut i: usize = 0;
    while i < v.len()
        invariant
            i <= v.len(),
            out.len() == i,
            forall|j: int| 0 <= j < i ==> (#[trigger] out[j]).m@ == v[j].m@ && sub_rt_view(out[j].row) == sub_rt_view(v[j].row),
        decreases v.len() - i,
    {
        out.push(WRow { row: v[i].row.dup(), m: v[i].m.dup() });
        i += 1;
    }
    out
}

proof fn lemma_wsum_view(a: Seq<WRow>, b: Seq<WRow>, f: spec_fn(SubRow) -> int)
    requires
        a.len() == b.len(),
        forall|i: int| 0 <= i < a.len() ==> (#[trigger] a[i]).m@ == b[i].m@ && sub_rt_view(a[i].row) == sub_rt_view(b[i].row),
    ensures wsum(a, f) == wsum(b, f),
{
    assert forall|i: int| 0 <= i < a.len() implies (|w: WRow| w.m@ * f(sub_rt_view(w.row)))(a[i]) == (|w: WRow| w.m@ * f(sub_rt_view(w.row)))(b[i]) by {}
    lemma_sum_map_eq(a, b, |w: WRow| w.m@ * f(sub_rt_view(w.row)));
}

proof fn lemma_sum_map_eq<A>(a: Seq<A>, b: Seq<A>, f: spec_fn(A) -> int)
    requires
        a.len() == b.len(),
        forall|i: int| 0 <= i < a.len() ==> f(#[trigger] a[i]) == f(b[i]),
    ensures sum(a, f) == sum(b, f),
    decreases a.len(),
{
    if a.len() > 0 {
        let (ta, tb) = (a.drop_last(), b.drop_last());
        assert forall|i: int| 0 <= i < ta.len() implies f(#[trigger] ta[i]) == f(tb[i]) by {
            assert(ta[i] == a[i] && tb[i] == b[i]);
        }
        lemma_sum_map_eq(ta, tb, f);
        assert(a =~= ta.push(a.last()));
        assert(b =~= tb.push(b.last()));
        lemma_sum_push(ta, a.last(), f);
        lemma_sum_push(tb, b.last(), f);
    }
}

/// Add a signed sub row to the buckets.
pub fn wbag_add(b: &mut Index<Vec<WRow>>, r: &SubRt, sg: i64, Ghost(phi): Ghost<Phi>)
    requires
        wbag_as(*old(b), phi),
        sg == 1 || sg == -1,
    ensures
        wbag_as(*final(b), phi_add(phi, sub_rt_view(*r), sg as int)),
{
    let ghost before = *b;
    let key = make_key1(r.adsh.as_slice());
    let found = b.get(&key);
    let mut nv: Vec<WRow> = match &found {
        Some(v) => dup_wrows(&**v),
        None => Vec::new(),
    };
    let ghost old_rows = wrows_at(before, key.view());
    proof {
        match &found {
            Some(v) => { lemma_wsum_view(nv@, v@, |s: SubRow| 0int); },
            None => {},
        }
    }
    let sgw = Wide::from_i128(sg as i128);
    let mut pos: usize = nv.len();
    let mut j: usize = 0;
    while j < nv.len()
        invariant
            j <= nv.len(),
            pos == nv.len() || (pos < nv.len() && pos < j && sub_rt_view(nv[pos as int].row) == sub_rt_view(*r)),
        decreases nv.len() - j,
    {
        if nv[j].row.same(r) && pos == nv.len() {
            pos = j;
        }
        j += 1;
    }
    let ghost nv0 = nv@;
    let ghost rv = sub_rt_view(*r);
    if pos < nv.len() {
        let m2 = nv[pos].m.add(&sgw);
        let _ = nv.remove(pos);
        if !m2.is_zero() {
            nv.push(WRow { row: r.dup(), m: m2 });
        }
        proof {
            assert forall|f: spec_fn(SubRow) -> int| #[trigger] wsum(nv@, f) == wsum(nv0, f) + sg as int * f(rv) by {
                let w = |x: WRow| x.m@ * f(sub_rt_view(x.row));
                lemma_sum_remove(nv0, pos as int, w);
                let (mp, fp) = (nv0[pos as int].m@, f(sub_rt_view(nv0[pos as int].row)));
                assert(fp == f(rv));
                assert(w(nv0[pos as int]) == mp * f(rv));
                if m2@ == 0 {
                    assert(nv@ =~= nv0.remove(pos as int));
                    assert(mp + sg as int == 0);
                    assert(mp * f(rv) == -(sg as int) * f(rv)) by (nonlinear_arith) requires mp + sg as int == 0;
                } else {
                    let last = nv@.last();
                    assert(nv@ =~= nv0.remove(pos as int).push(last));
                    lemma_sum_push(nv0.remove(pos as int), last, w);
                    assert(w(last) == (mp + sg as int) * f(rv));
                    assert((mp + sg as int) * f(rv) == mp * f(rv) + sg as int * f(rv)) by (nonlinear_arith);
                }
            }
        }
    } else {
        nv.push(WRow { row: r.dup(), m: sgw });
        proof {
            assert forall|f: spec_fn(SubRow) -> int| #[trigger] wsum(nv@, f) == wsum(nv0, f) + sg as int * f(rv) by {
                let w = |x: WRow| x.m@ * f(sub_rt_view(x.row));
                let last = nv@.last();
                assert(nv@ =~= nv0.push(last));
                lemma_sum_push(nv0, last, w);
            }
        }
    }
    b.put(key.share(), Arc::new(nv));
    proof {
        let phi2 = phi_add(phi, rv, sg as int);
        assert forall|k: KeyView, f: spec_fn(SubRow) -> int| #[trigger] wsum(wrows_at(*b, k), f) == phi2(skw(k, f)) by {
            if k == key.view() {
                assert(skw(k, f)(rv) == f(rv));
                assert(old_rows == (match &found { Some(v) => v@, None => Seq::empty() }));
                assert(wsum(old_rows, f) == phi(skw(k, f)));
                match &found {
                    Some(v) => { lemma_wsum_view(nv0, v@, f); },
                    None => { assert(nv0 =~= Seq::<WRow>::empty()); },
                }
            } else {
                assert(wrows_at(*b, k) == wrows_at(before, k));
                assert(skw(k, f)(rv) == 0);
            }
        }
    }
}

}

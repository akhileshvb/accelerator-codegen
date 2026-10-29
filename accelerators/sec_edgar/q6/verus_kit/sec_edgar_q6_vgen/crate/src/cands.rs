// The groups of a class (the keys with a positive count) as a list without repetition.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::lemma_flat_map_contains;
use crate::gen_spec::*;
use crate::model::*;
use crate::algebra::*;
use crate::answer::*;

verus! {

pub open spec fn is_cand(db: Db, p: Params, k: GK) -> bool {
    k_params(k) == p && cntk(db, k) > 0
}

pub open spec fn tkey(p: Params, t: Tuple) -> GK {
    GK { fy: p.p1, stmt: p.p2, uom: p.p3, name: t.s_.name, tag: t.n.tag->0, plabel: t.p.plabel }
}

proof fn lemma_tuples_where(db: Db, p: Params, t: Tuple)
    requires tuples(db, p).contains(t),
    ensures where_clause(db, p, t.n, t.s_, t.p),
{
    reveal(tuples);
    let g0 = |n: NumRow| tuples_1(db, p, n);
    lemma_flat_map_contains(db.num, g0, t);
    let i = choose|i: int| 0 <= i < db.num.len() && g0(db.num[i]).contains(t);
    let n = db.num[i];
    reveal(tuples_1);
    let g1 = |s: SubRow| tuples_2(db, p, n, s);
    lemma_flat_map_contains(db.sub, g1, t);
    let j = choose|j: int| 0 <= j < db.sub.len() && g1(db.sub[j]).contains(t);
    let s = db.sub[j];
    reveal(tuples_2);
    let g2 = |pp: PreRow| tuples_3(db, p, n, s, pp);
    lemma_flat_map_contains(db.pre, g2, t);
    let q = choose|q: int| 0 <= q < db.pre.len() && g2(db.pre[q]).contains(t);
    reveal(tuples_3);
    assert(tuples_3(db, p, n, s, db.pre[q]).contains(t));
    assert(where_clause(db, p, n, s, db.pre[q]));
    assert(t == Tuple { n, s_: s, p: db.pre[q] });
}

/// The count of a class's group is the spec's.
pub proof fn lemma_cnt_gc(db: Db, p: Params, k: GK)
    requires k_params(k) == p,
    ensures cntk(db, k) == group_count(db, p, k.name, Some(p.p2), Some(k.tag), k.plabel),
{
    let one = |t: Tuple| 1int;
    lemma_agg_cnt(db, p, k, one);
    reveal(group_sum);
    reveal(group_count);
    let a = |t: Tuple| if t.s_.name == k.name && t.p.stmt == Some(p.p2) && t.n.tag == Some(k.tag) && t.p.plabel == k.plabel { 1int } else { 0 };
    let b = |t: Tuple| if t.s_.name == k.name && t.p.stmt == Some(p.p2) && t.n.tag == Some(k.tag) && t.p.plabel == k.plabel { one(t) } else { 0 };
    assert forall|t: Tuple| #[trigger] a(t) == b(t) by {}
    lemma_sum_ext_all(tuples(db, p), a, b);
}

proof fn lemma_agg_cnt(db: Db, p: Params, k: GK, f: spec_fn(Tuple) -> int)
    requires k_params(k) == p, forall|t: Tuple| #[trigger] f(t) == 1int,
    ensures group_sum(db, p, k.name, Some(p.p2), Some(k.tag), k.plabel, f) == cntk(db, k),
{
    lemma_group_sum_nested(db, p, k.name, Some(p.p2), Some(k.tag), k.plabel, f);
    assert(is_key(p, Some(p.p2), Some(k.tag)));
    assert(k_of(p, k.name, Some(k.tag), k.plabel) == k);
    let h = |n: NumRow, s: SubRow, pp: PreRow| if jk(n, s, pp) && key_of(n, s, pp) == k { f(Tuple { n, s_: s, p: pp }) } else { 0 };
    assert forall|n: NumRow, s: SubRow, pp: PreRow| #[trigger] h(n, s, pp) == icnt(k)(n, s, pp) by {}
    crate::glue_proofs::lemma_gdb_ext(db, h, icnt(k));
}

pub proof fn lemma_cand_list(db: Db, p: Params) -> (kl: Seq<GK>)
    ensures
        no_dups(kl),
        forall|i: int| 0 <= i < kl.len() ==> is_cand(db, p, #[trigger] kl[i]),
        forall|k: GK| is_cand(db, p, k) ==> kl.contains(k),
{
    let ts = tuples(db, p);
    let keys = ts.map_values(|t: Tuple| tkey(p, t));
    lemma_dedup(keys);
    let kl = dedup(keys);
    assert forall|i: int| 0 <= i < kl.len() implies is_cand(db, p, #[trigger] kl[i]) by {
        let k = kl[i];
        assert(dedup(keys).contains(k));
        assert(keys.contains(k));
        let j = choose|j: int| 0 <= j < keys.len() && keys[j] == k;
        let t = ts[j];
        assert(ts.contains(t));
        lemma_tuples_where(db, p, t);
        assert(k_params(k) == p);
        lemma_cnt_gc(db, p, k);
        let w = |u: Tuple| if u.s_.name == k.name && u.p.stmt == Some(p.p2) && u.n.tag == Some(k.tag) && u.p.plabel == k.plabel { 1int } else { 0 };
        reveal(group_count);
        assert(w(t) == 1);
        assert forall|q: int| 0 <= q < ts.len() implies w(#[trigger] ts[q]) >= 0 by {}
        lemma_sum_pos(ts, w, j);
    }
    assert forall|k: GK| is_cand(db, p, k) implies kl.contains(k) by {
        lemma_cnt_gc(db, p, k);
        reveal(group_count);
        let w = |u: Tuple| if u.s_.name == k.name && u.p.stmt == Some(p.p2) && u.n.tag == Some(k.tag) && u.p.plabel == k.plabel { 1int } else { 0 };
        let j = lemma_sum_nonzero_witness(ts, w);
        let t = ts[j];
        assert(ts.contains(t));
        lemma_tuples_where(db, p, t);
        assert(0 <= j < keys.len());
        assert(keys[j] == tkey(p, t));
        assert(tkey(p, t) == k);
        assert(keys.contains(k));
    }
    kl
}

} // verus!

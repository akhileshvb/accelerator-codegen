// Row positions of a signed list grouped by join key, in ordered indexes.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::esc::*;
use crate::model::*;
use crate::algebra::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;

verus! {

pub struct Bucket {
    pub pos: Vec<usize>,
}

impl Bucket {
    pub open spec fn ints(self) -> Seq<int> {
        self.pos@.map_values(|x: usize| x as int)
    }
}

/// Positions of the rows m selects.
pub open spec fn pos_where<R>(l: Seq<(R, int)>, m: spec_fn(R) -> bool) -> Seq<int>
    decreases l.len(),
{
    if l.len() == 0 { Seq::empty() } else {
        pos_where(l.drop_last(), m) + if m(l.last().0) { seq![l.len() - 1] } else { Seq::empty() }
    }
}

pub open spec fn bsum<R>(l: Seq<(R, int)>, pos: Seq<int>, f: spec_fn(R) -> int) -> int {
    sum(pos, |j: int| l[j].1 * f(l[j].0))
}

/// When f vanishes off m, the weighted sum runs over the bucket alone.
pub proof fn lemma_bucket_sum<R>(l: Seq<(R, int)>, m: spec_fn(R) -> bool, f: spec_fn(R) -> int)
    requires forall|r: R| !#[trigger] m(r) ==> f(r) == 0,
    ensures wsum(l, f) == bsum(l, pos_where(l, m), f),
    decreases l.len(),
{
    if l.len() > 0 {
        let t = l.drop_last();
        lemma_bucket_sum(t, m, f);
        assert(l =~= t.push(l.last()));
        lemma_sum_push(t, l.last(), |x: (R, int)| x.1 * f(x.0));
        let g = |j: int| l[j].1 * f(l[j].0);
        let gt = |j: int| t[j].1 * f(t[j].0);
        let p = pos_where(t, m);
        lemma_pos_in_range(t, m);
        assert forall|i: int| 0 <= i < p.len() implies g(#[trigger] p[i]) == gt(p[i]) by {
            assert(p[i] < t.len());
        }
        lemma_sum_ext(p, g, gt);
        if m(l.last().0) {
            lemma_sum_push(p, l.len() - 1, g);
            assert(pos_where(l, m) =~= p.push(l.len() - 1));
        } else {
            assert(f(l.last().0) == 0);
            assert(l.last().1 * f(l.last().0) == 0) by (nonlinear_arith) requires f(l.last().0) == 0;
            assert(pos_where(l, m) =~= p);
        }
    }
}

pub proof fn lemma_pos_in_range<R>(l: Seq<(R, int)>, m: spec_fn(R) -> bool)
    ensures forall|i: int| 0 <= i < pos_where(l, m).len() ==> 0 <= #[trigger] pos_where(l, m)[i] < l.len(),
    decreases l.len(),
{
    if l.len() > 0 {
        lemma_pos_in_range(l.drop_last(), m);
        let p = pos_where(l.drop_last(), m);
        if m(l.last().0) {
            assert(pos_where(l, m) =~= p.push(l.len() - 1));
        } else {
            assert(pos_where(l, m) =~= p);
        }
    }
}

// ── sub rows by adsh ──

pub open spec fn sub_bk(r: SubRow) -> KeyView {
    (0int, esc(r.adsh))
}

pub open spec fn m_sub(kv: KeyView) -> spec_fn(SubRow) -> bool {
    |r: SubRow| sub_bk(r) == kv
}

pub open spec fn buckets_ok<R>(idx: Index<Bucket>, l: Seq<(R, int)>, mk: spec_fn(KeyView) -> spec_fn(R) -> bool) -> bool {
    idx.wf() && forall|kv: KeyView| #![trigger idx.at(kv)]
        match idx.at(kv) {
            Some(b) => b.ints() == pos_where(l, mk(kv)) && b.pos@.len() > 0,
            None => pos_where(l, mk(kv)).len() == 0,
        }
}

pub fn build_sub(ls: &Vec<SubDelta>) -> (idx: Index<Bucket>)
    ensures buckets_ok(idx, sub_delta_seq_view(ls@), |kv: KeyView| m_sub(kv)),
{
    let mut idx: Index<Bucket> = Index::new();
    let mut i = 0;
    let ghost view = sub_delta_seq_view(ls@);
    proof {
        assert forall|kv: KeyView| #![trigger idx.at(kv)] pos_where(view.take(0), m_sub(kv)).len() == 0 by {
            assert(view.take(0) =~= Seq::<(SubRow, int)>::empty());
        }
    }
    while i < ls.len()
        invariant
            i <= ls.len(),
            view == sub_delta_seq_view(ls@),
            buckets_ok(idx, view.take(i as int), |kv: KeyView| m_sub(kv)),
        decreases ls.len() - i,
    {
        let key = Key::composite(0, {
            let mut t = Vec::new();
            append_esc(&mut t, ls[i].row.adsh.as_slice());
            t
        });
        let ghost kv = key.view();
        let old_b = idx.get(&key);
        let mut pos: Vec<usize> = Vec::new();
        match &old_b {
            Some(b) => {
                let mut t = 0;
                while t < b.pos.len()
                    invariant t <= b.pos.len(), pos@ == b.pos@.take(t as int),
                    decreases b.pos.len() - t,
                {
                    pos.push(b.pos[t]);
                    proof { assert(b.pos@.take(t as int + 1) =~= b.pos@.take(t as int).push(b.pos[t as int])); }
                    t += 1;
                }
                proof { assert(b.pos@.take(t as int) =~= b.pos@); }
            }
            None => {}
        }
        pos.push(i);
        let ghost before = idx;
        let ghost taken = view.take(i as int + 1);
        proof {
            assert(taken.drop_last() =~= view.take(i as int));
            assert(taken.last() == view[i as int]);
            assert(kv == sub_bk(view[i as int].0));
        }
        idx.put(key, Arc::new(Bucket { pos }));
        proof {
            assert forall|kv2: KeyView| #![trigger idx.at(kv2)]
                match idx.at(kv2) {
                    Some(b) => b.ints() == pos_where(taken, m_sub(kv2)) && b.pos@.len() > 0,
                    None => pos_where(taken, m_sub(kv2)).len() == 0,
                } by {
                if kv2 == kv {
                    let p = pos_where(view.take(i as int), m_sub(kv));
                    assert(pos_where(taken, m_sub(kv)) =~= p.push(i as int));
                    match old_b {
                        Some(b) => { assert(b.ints() == p); assert(idx.at(kv)->0.ints() =~= p.push(i as int)); }
                        None => { assert(p.len() == 0); assert(idx.at(kv)->0.ints() =~= p.push(i as int)); }
                    }
                } else {
                    assert(!m_sub(kv2)(view[i as int].0));
                    assert(pos_where(taken, m_sub(kv2)) =~= pos_where(view.take(i as int), m_sub(kv2)));
                }
            }
        }
        i += 1;
    }
    proof { assert(view.take(i as int) =~= view); }
    idx
}

// ── pre rows by (adsh, tag, version) ──

pub open spec fn pre_key3(a: Seq<u8>, t: Seq<u8>, v: Seq<u8>) -> KeyView {
    (0int, esc(a) + esc(t) + esc(v))
}

pub open spec fn pre_bk(r: PreRow) -> Option<KeyView> {
    if r.adsh is Some && r.tag is Some && r.version is Some { Some(pre_key3(r.adsh->0, r.tag->0, r.version->0)) } else { None }
}

pub open spec fn m_pre(kv: KeyView) -> spec_fn(PreRow) -> bool {
    |r: PreRow| pre_bk(r) == Some(kv)
}

pub proof fn lemma_key3_inj(a1: Seq<u8>, t1: Seq<u8>, v1: Seq<u8>, a2: Seq<u8>, t2: Seq<u8>, v2: Seq<u8>)
    requires pre_key3(a1, t1, v1) == pre_key3(a2, t2, v2),
    ensures a1 == a2, t1 == t2, v1 == v2,
{
    assert(esc(a1) + esc(t1) + esc(v1) =~= esc(a1) + (esc(t1) + esc(v1)));
    assert(esc(a2) + esc(t2) + esc(v2) =~= esc(a2) + (esc(t2) + esc(v2)));
    lemma_esc_split(a1, esc(t1) + esc(v1), a2, esc(t2) + esc(v2));
    lemma_esc_split(t1, esc(v1), t2, esc(v2));
    lemma_esc_split(v1, Seq::empty(), v2, Seq::empty());
}

pub fn build_pre(lp: &Vec<PreDelta>) -> (idx: Index<Bucket>)
    ensures buckets_ok(idx, pre_delta_seq_view(lp@), |kv: KeyView| m_pre(kv)),
{
    let mut idx: Index<Bucket> = Index::new();
    let mut i = 0;
    let ghost view = pre_delta_seq_view(lp@);
    proof {
        assert forall|kv: KeyView| #![trigger idx.at(kv)] pos_where(view.take(0), m_pre(kv)).len() == 0 by {
            assert(view.take(0) =~= Seq::<(PreRow, int)>::empty());
        }
    }
    while i < lp.len()
        invariant
            i <= lp.len(),
            view == pre_delta_seq_view(lp@),
            buckets_ok(idx, view.take(i as int), |kv: KeyView| m_pre(kv)),
        decreases lp.len() - i,
    {
        let r = &lp[i].row;
        let ghost taken = view.take(i as int + 1);
        proof {
            assert(taken.drop_last() =~= view.take(i as int));
            assert(taken.last() == view[i as int]);
        }
        if r.adsh__null != 0 || r.tag__null != 0 || r.version__null != 0 {
            proof {
                assert forall|kv2: KeyView| #![trigger idx.at(kv2)] pos_where(taken, m_pre(kv2)) =~= pos_where(view.take(i as int), m_pre(kv2)) by {
                    assert(!m_pre(kv2)(view[i as int].0));
                }
            }
            i += 1;
            continue;
        }
        let mut text = Vec::new();
        append_esc(&mut text, r.adsh.as_slice());
        append_esc(&mut text, r.tag.as_slice());
        append_esc(&mut text, r.version.as_slice());
        let key = Key::composite(0, text);
        let ghost kv = key.view();
        proof { assert(Some(kv) == pre_bk(view[i as int].0)); }
        let old_b = idx.get(&key);
        let mut pos: Vec<usize> = Vec::new();
        match &old_b {
            Some(b) => {
                let mut t = 0;
                while t < b.pos.len()
                    invariant t <= b.pos.len(), pos@ == b.pos@.take(t as int),
                    decreases b.pos.len() - t,
                {
                    pos.push(b.pos[t]);
                    proof { assert(b.pos@.take(t as int + 1) =~= b.pos@.take(t as int).push(b.pos[t as int])); }
                    t += 1;
                }
                proof { assert(b.pos@.take(t as int) =~= b.pos@); }
            }
            None => {}
        }
        pos.push(i);
        idx.put(key, Arc::new(Bucket { pos }));
        proof {
            assert forall|kv2: KeyView| #![trigger idx.at(kv2)]
                match idx.at(kv2) {
                    Some(b) => b.ints() == pos_where(taken, m_pre(kv2)) && b.pos@.len() > 0,
                    None => pos_where(taken, m_pre(kv2)).len() == 0,
                } by {
                if kv2 == kv {
                    let p = pos_where(view.take(i as int), m_pre(kv));
                    assert(pos_where(taken, m_pre(kv)) =~= p.push(i as int));
                    match old_b {
                        Some(b) => { assert(b.ints() == p); assert(idx.at(kv)->0.ints() =~= p.push(i as int)); }
                        None => { assert(p.len() == 0); assert(idx.at(kv)->0.ints() =~= p.push(i as int)); }
                    }
                } else {
                    assert(!m_pre(kv2)(view[i as int].0));
                    assert(pos_where(taken, m_pre(kv2)) =~= pos_where(view.take(i as int), m_pre(kv2)));
                }
            }
        }
        i += 1;
    }
    proof { assert(view.take(i as int) =~= view); }
    idx
}

} // verus!

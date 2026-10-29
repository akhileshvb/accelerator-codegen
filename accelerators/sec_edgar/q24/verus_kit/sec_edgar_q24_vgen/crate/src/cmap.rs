// Counts of pre rows per (adsh, tag, version): the join side of the anti-join.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::esc::*;
use crate::model::*;
use crate::algebra::*;
use verus_prooflib::lemma_sum_ones;

verus! {

pub type CMap = Index<i128>;

pub open spec fn cm_at(m: CMap, a: Seq<u8>, t: Seq<u8>, v: Seq<u8>) -> int {
    match m.at(key3(a, t, v)) { Some(c) => c as int, None => 0 }
}

/// A count with room to spare in an i128.
pub open spec fn small(x: int) -> bool {
    -1267650600228229401496703205376 < x && x < 1267650600228229401496703205376
}

pub open spec fn hit(p: PreRow, a: Seq<u8>, t: Seq<u8>, v: Seq<u8>) -> bool {
    p.adsh is Some && p.tag == Some(t) && p.version == Some(v) && p.adsh == Some(a)
}

/// The pre rows with key (a, t, v).
pub open spec fn pc(rp: Seq<PreRow>, a: Seq<u8>, t: Seq<u8>, v: Seq<u8>) -> int {
    sum(rp, |p: PreRow| ind(hit(p, a, t, v)))
}

/// The signed changed pre rows with key (a, t, v).
pub open spec fn dk(dp: Seq<(PreRow, int)>, a: Seq<u8>, t: Seq<u8>, v: Seq<u8>) -> int {
    sum(dp, |d: (PreRow, int)| d.1 * ind(hit(d.0, a, t, v)))
}

/// A row with the whole key joins exactly the pre rows with that key.
pub proof fn lemma_mc_pc(rp: Seq<PreRow>, dp: Seq<(PreRow, int)>, n: NumRow, a: Seq<u8>, t: Seq<u8>, v: Seq<u8>)
    requires n.adsh == Some(a), n.tag == Some(t), n.version == Some(v),
    ensures
        mc(rp, n) == pc(rp, a, t, v),
        dm(dp, n) == dk(dp, a, t, v),
{
    assert forall|p: PreRow| #[trigger] mw(n)(p) == ind(hit(p, a, t, v)) by {}
    lemma_sum_ext_all(rp, mw(n), |p: PreRow| ind(hit(p, a, t, v)));
    assert forall|d: (PreRow, int)| #[trigger] (d.1 * mw(n)(d.0)) == d.1 * ind(hit(d.0, a, t, v)) by {}
    lemma_sum_ext_all(dp, |d: (PreRow, int)| d.1 * mw(n)(d.0), |d: (PreRow, int)| d.1 * ind(hit(d.0, a, t, v)));
}

/// A row without a whole key joins nothing.
pub proof fn lemma_mc_none(rp: Seq<PreRow>, dp: Seq<(PreRow, int)>, n: NumRow)
    requires n.adsh is None || n.tag is None || n.version is None,
    ensures mc(rp, n) == 0, dm(dp, n) == 0,
{
    assert forall|p: PreRow| #[trigger] mw(n)(p) == 0 by {}
    lemma_sum_zero(rp, mw(n));
    let z = |d: (PreRow, int)| d.1 * mw(n)(d.0);
    assert forall|d: (PreRow, int)| #[trigger] z(d) == 0 by {}
    lemma_sum_zero(dp, z);
}

pub proof fn lemma_pc_le(rp: Seq<PreRow>, a: Seq<u8>, t: Seq<u8>, v: Seq<u8>)
    ensures 0 <= pc(rp, a, t, v) <= rp.len(),
{
    lemma_sum_ones(rp);
    lemma_sum_le(rp, |p: PreRow| ind(hit(p, a, t, v)), |p: PreRow| 1int);
    assert forall|i: int| 0 <= i < rp.len() implies (|p: PreRow| ind(hit(p, a, t, v)))(#[trigger] rp[i]) >= 0 by {}
    lemma_sum_nonneg(rp, |p: PreRow| ind(hit(p, a, t, v)));
}

/// Adds d to the count of key (a, t, v).
pub fn cm_add(m: &mut CMap, a: &[u8], t: &[u8], v: &[u8], d: i128)
    requires
        old(m).wf(),
        small(cm_at(*old(m), a@, t@, v@) + d),
    ensures
        final(m).wf(),
        forall|a2: Seq<u8>, t2: Seq<u8>, v2: Seq<u8>| #[trigger] cm_at(*final(m), a2, t2, v2)
            == cm_at(*old(m), a2, t2, v2) + if a2 == a@ && t2 == t@ && v2 == v@ { d as int } else { 0 },
{
    let ghost old_m = *m;
    let key = key3_of(a, t, v);
    let c: i128 = match m.get(&key) {
        Some(c) => *c,
        None => 0,
    };
    proof { assert(c as int == cm_at(old_m, a@, t@, v@)); }
    let ghost kv = key.view();
    m.put(key, Arc::new(c + d));
    proof {
        assert forall|a2: Seq<u8>, t2: Seq<u8>, v2: Seq<u8>| #[trigger] cm_at(*m, a2, t2, v2)
            == cm_at(old_m, a2, t2, v2) + if a2 == a@ && t2 == t@ && v2 == v@ { d as int } else { 0 } by {
            if key3(a2, t2, v2) == kv { lemma_key3_inj(a2, t2, v2, a@, t@, v@); }
        }
    }
}

/// The pre rows with a whole key, counted per key.
pub fn count_pres(rows: &Vec<PreRt>) -> (m: CMap)
    ensures
        m.wf(),
        forall|a: Seq<u8>, t: Seq<u8>, v: Seq<u8>| #[trigger] cm_at(m, a, t, v) == pc(pre_rt_seq_view(rows@), a, t, v),
{
    let mut m: CMap = Index::new();
    let mut i = 0;
    while i < rows.len()
        invariant
            i <= rows.len(), m.wf(),
            forall|a: Seq<u8>, t: Seq<u8>, v: Seq<u8>| #[trigger] cm_at(m, a, t, v) == pc(pre_rt_seq_view(rows@.take(i as int)), a, t, v),
            forall|a: Seq<u8>, t: Seq<u8>, v: Seq<u8>| #[trigger] cm_at(m, a, t, v) <= i,
        decreases rows.len() - i,
    {
        let r = &rows[i];
        let ghost before = m;
        proof {
            let tk = rows@.take(i as int + 1);
            assert(tk =~= rows@.take(i as int).push(rows@[i as int]));
            assert(pre_rt_seq_view(tk) =~= pre_rt_seq_view(rows@.take(i as int)).push(pre_rt_view(rows@[i as int])));
        }
        if r.adsh__null == 0 && r.tag__null == 0 && r.version__null == 0 {
            proof { lemma_pc_le(pre_rt_seq_view(rows@.take(i as int)), r.adsh@, r.tag@, r.version@); }
            cm_add(&mut m, r.adsh.as_slice(), r.tag.as_slice(), r.version.as_slice(), 1);
        }
        proof {
            let vs = pre_rt_seq_view(rows@.take(i as int));
            let x = pre_rt_view(rows@[i as int]);
            assert forall|a: Seq<u8>, t: Seq<u8>, v: Seq<u8>| #[trigger] cm_at(m, a, t, v) == pc(pre_rt_seq_view(rows@.take(i as int + 1)), a, t, v) by {
                lemma_sum_push(vs, x, |p: PreRow| ind(hit(p, a, t, v)));
                assert(pre_rt_seq_view(rows@.take(i as int + 1)) =~= vs.push(x));
            }
            assert forall|a: Seq<u8>, t: Seq<u8>, v: Seq<u8>| #[trigger] cm_at(m, a, t, v) <= i + 1 by {}
        }
        i += 1;
    }
    proof { assert(rows@.take(i as int) =~= rows@); }
    m
}

/// The old counts of the filings: the new ones less the signed changed pre rows.
pub fn old_counts(c2: &CMap, dp: &Vec<PreDelta>, Ghost(rp): Ghost<Seq<PreRow>>) -> (m: CMap)
    requires
        c2.wf(),
        rp.len() <= 18446744073709551615,
        forall|a: Seq<u8>, t: Seq<u8>, v: Seq<u8>| #[trigger] cm_at(*c2, a, t, v) == pc(rp, a, t, v),
        forall|i: int| 0 <= i < dp.len() ==> (#[trigger] dp@[i]).sign == 1 || dp@[i].sign == -1,
    ensures
        m.wf(),
        forall|a: Seq<u8>, t: Seq<u8>, v: Seq<u8>| #[trigger] cm_at(m, a, t, v) == pc(rp, a, t, v) - dk(pre_delta_seq_view(dp@), a, t, v),
{
    let mut m = c2.share();
    proof {
        assert forall|a: Seq<u8>, t: Seq<u8>, v: Seq<u8>| #[trigger] cm_at(m, a, t, v) == pc(rp, a, t, v) - dk(pre_delta_seq_view(dp@.take(0)), a, t, v) by {
            assert(m.at(key3(a, t, v)) == c2.at(key3(a, t, v)));
            assert(cm_at(*c2, a, t, v) == pc(rp, a, t, v));
            assert(cm_at(m, a, t, v) == cm_at(*c2, a, t, v));
            assert(dp@.take(0) =~= Seq::<PreDelta>::empty());
            assert(pre_delta_seq_view(dp@.take(0)) =~= Seq::<(PreRow, int)>::empty());
            lemma_sum_empty(Seq::<(PreRow, int)>::empty(), |x: (PreRow, int)| x.1 * ind(hit(x.0, a, t, v)));
            assert(dk(pre_delta_seq_view(dp@.take(0)), a, t, v) == 0);
        }
    }
    let mut i = 0;
    while i < dp.len()
        invariant
            i <= dp.len(), m.wf(), rp.len() <= 18446744073709551615,
            forall|i: int| 0 <= i < dp.len() ==> (#[trigger] dp@[i]).sign == 1 || dp@[i].sign == -1,
            forall|a: Seq<u8>, t: Seq<u8>, v: Seq<u8>| #[trigger] cm_at(m, a, t, v) == pc(rp, a, t, v) - dk(pre_delta_seq_view(dp@.take(i as int)), a, t, v),
            forall|a: Seq<u8>, t: Seq<u8>, v: Seq<u8>| #[trigger] pc(rp, a, t, v) - i <= cm_at(m, a, t, v) <= pc(rp, a, t, v) + i,
        decreases dp.len() - i,
    {
        let d = &dp[i];
        let r = &d.row;
        proof {
            let tk = dp@.take(i as int + 1);
            assert(tk =~= dp@.take(i as int).push(dp@[i as int]));
            assert(pre_delta_seq_view(tk) =~= pre_delta_seq_view(dp@.take(i as int)).push((pre_rt_view(*r), d.sign as int)));
            assert forall|a: Seq<u8>, t: Seq<u8>, v: Seq<u8>| #[trigger] dk(pre_delta_seq_view(tk), a, t, v)
                == dk(pre_delta_seq_view(dp@.take(i as int)), a, t, v) + d.sign * ind(hit(pre_rt_view(*r), a, t, v)) by {
                lemma_sum_push(pre_delta_seq_view(dp@.take(i as int)), (pre_rt_view(*r), d.sign as int), |x: (PreRow, int)| x.1 * ind(hit(x.0, a, t, v)));
            }
        }
        if r.adsh__null == 0 && r.tag__null == 0 && r.version__null == 0 {
            let (sa, st, sv) = (r.adsh.as_slice(), r.tag.as_slice(), r.version.as_slice());
            let dd: i128 = -(d.sign as i128);
            proof {
                lemma_pc_le(rp, sa@, st@, sv@);
                assert(i < dp.len());
                assert(i <= 18446744073709551615);
                assert(dp@[i as int].sign == 1 || dp@[i as int].sign == -1);
                assert(d.sign == 1 || d.sign == -1);
                assert(pc(rp, sa@, st@, sv@) <= rp.len());
                assert(cm_at(m, sa@, st@, sv@) <= rp.len() + i);
                assert(cm_at(m, sa@, st@, sv@) >= -(i as int));
                assert(dd == 1 || dd == -1);
                assert(small(cm_at(m, sa@, st@, sv@) + dd as int));
            }
            cm_add(&mut m, sa, st, sv, dd);
        }
        proof {
            assert forall|a: Seq<u8>, t: Seq<u8>, v: Seq<u8>| #[trigger] cm_at(m, a, t, v) == pc(rp, a, t, v) - dk(pre_delta_seq_view(dp@.take(i as int + 1)), a, t, v) by {
                assert(pre_rt_view(*r).adsh is Some == (r.adsh__null == 0));
            }
        }
        i += 1;
    }
    proof { assert(dp@.take(i as int) =~= dp@); }
    m
}

} // verus!

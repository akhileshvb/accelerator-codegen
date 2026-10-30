// The accumulation loops: a transaction's (or a scan's) num rows, weighted by their anti-join
// membership, added to the per-group contributions.
#![allow(unused_imports)]
#![verifier::loop_isolation(false)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::algebra::*;
use crate::esc::*;
use crate::accmap::*;
use crate::cmap::*;
use crate::rows::*;
use crate::wideval::*;

verus! {

pub open spec fn full_key(n: NumRow) -> bool {
    n.adsh is Some && n.tag is Some && n.version is Some
}

/// n has no pre row according to the counts c.
pub open spec fn um_c(c: CMap, n: NumRow) -> bool {
    if full_key(n) { cm_at(c, n.adsh->0, n.tag->0, n.version->0) == 0 } else { true }
}

pub fn unmatched(c: &CMap, r: &NumRt) -> (b: bool)
    requires c.wf(),
    ensures b == um_c(*c, num_rt_view(*r)),
{
    if r.adsh__null == 0 && r.tag__null == 0 && r.version__null == 0 {
        let key = key3_of(r.adsh.as_slice(), r.tag.as_slice(), r.version.as_slice());
        match c.get(&key) {
            Some(x) => *x == 0,
            None => true,
        }
    } else {
        true
    }
}

pub open spec fn wt(c: CMap, n: NumRow) -> int {
    ind(um_c(c, n))
}

/// The counts of the rescanned filings' pre rows (c2) and of their old rows (c1) decide z2 and z1.
pub open spec fn counts_ok(c2: CMap, c1: CMap, rp: Seq<PreRow>, dp: Seq<(PreRow, int)>) -> bool {
    &&& c2.wf() && c1.wf()
    &&& forall|a: Seq<u8>, t: Seq<u8>, v: Seq<u8>| #[trigger] cm_at(c2, a, t, v) == pc(rp, a, t, v)
    &&& forall|a: Seq<u8>, t: Seq<u8>, v: Seq<u8>| #[trigger] cm_at(c1, a, t, v) == pc(rp, a, t, v) - dk(dp, a, t, v)
}

pub proof fn lemma_wt(c2: CMap, c1: CMap, rp: Seq<PreRow>, dp: Seq<(PreRow, int)>, n: NumRow)
    requires counts_ok(c2, c1, rp, dp),
    ensures wt(c2, n) == z2(rp, n), wt(c1, n) == z1(rp, dp, n),
{
    if full_key(n) {
        lemma_mc_pc(rp, dp, n, n.adsh->0, n.tag->0, n.version->0);
    } else {
        lemma_mc_none(rp, dp, n);
    }
}

pub proof fn lemma_wt_build(c: CMap, rp: Seq<PreRow>, n: NumRow)
    requires c.wf(), forall|a: Seq<u8>, t: Seq<u8>, v: Seq<u8>| #[trigger] cm_at(c, a, t, v) == pc(rp, a, t, v),
    ensures wt(c, n) == z2(rp, n),
{
    if full_key(n) {
        lemma_mc_pc(rp, Seq::empty(), n, n.adsh->0, n.tag->0, n.version->0);
    } else {
        lemma_mc_none(rp, Seq::empty(), n);
    }
}

pub open spec fn rescan_w(rp: Seq<PreRow>, dp: Seq<(PreRow, int)>) -> spec_fn(NumRow) -> int {
    |n: NumRow| z2(rp, n) - z1(rp, dp, n)
}

/// Rescanned num rows: the change of membership.
pub fn acc_rescan(tab: &Pow2Tab, acc: &mut Index<AccEnt>, rows: &Vec<NumRt>, c2: &CMap, c1: &CMap, Ghost(rp): Ghost<Seq<PreRow>>, Ghost(dp): Ghost<Seq<(PreRow, int)>>)
    requires
        tab.ok(), acc_ok(*old(acc)), counts_ok(*c2, *c1, rp, dp),
    ensures
        acc_ok(*final(acc)),
        forall|k: GK| #![trigger acc_c(*final(acc), k)] acc_c(*final(acc), k) == acc_c(*old(acc), k) + sum(num_rt_seq_view(rows@), |n: NumRow| rescan_w(rp, dp)(n) * ic(k)(n)),
        forall|k: GK| #![trigger acc_s(*final(acc), k)] acc_s(*final(acc), k) == acc_s(*old(acc), k) + sum(num_rt_seq_view(rows@), |n: NumRow| rescan_w(rp, dp)(n) * iv(k)(n)),
        forall|k: GK| #![trigger acc_a(*final(acc), k)] acc_a(*final(acc), k) == acc_a(*old(acc), k) + sum(num_rt_seq_view(rows@), |n: NumRow| rescan_w(rp, dp)(n) * ia(k)(n)),
{
    let ghost acc0 = *acc;
    let mut i = 0;
    proof {
        assert forall|k: GK| #![trigger acc_c(*acc, k)] acc_c(*acc, k) == acc_c(acc0, k) + sum(num_rt_seq_view(rows@.take(0)), |n: NumRow| rescan_w(rp, dp)(n) * ic(k)(n)) by {
            assert(num_rt_seq_view(rows@.take(0)) =~= Seq::<NumRow>::empty());
            lemma_sum_empty(Seq::<NumRow>::empty(), |n: NumRow| rescan_w(rp, dp)(n) * ic(k)(n));
        }
        assert forall|k: GK| #![trigger acc_s(*acc, k)] acc_s(*acc, k) == acc_s(acc0, k) + sum(num_rt_seq_view(rows@.take(0)), |n: NumRow| rescan_w(rp, dp)(n) * iv(k)(n)) by {
            assert(num_rt_seq_view(rows@.take(0)) =~= Seq::<NumRow>::empty());
            lemma_sum_empty(Seq::<NumRow>::empty(), |n: NumRow| rescan_w(rp, dp)(n) * iv(k)(n));
        }
        assert forall|k: GK| #![trigger acc_a(*acc, k)] acc_a(*acc, k) == acc_a(acc0, k) + sum(num_rt_seq_view(rows@.take(0)), |n: NumRow| rescan_w(rp, dp)(n) * ia(k)(n)) by {
            assert(num_rt_seq_view(rows@.take(0)) =~= Seq::<NumRow>::empty());
            lemma_sum_empty(Seq::<NumRow>::empty(), |n: NumRow| rescan_w(rp, dp)(n) * ia(k)(n));
        }
    }
    while i < rows.len()
        invariant
            i <= rows.len(), tab.ok(), acc_ok(*acc), counts_ok(*c2, *c1, rp, dp),
            forall|k: GK| #![trigger acc_c(*acc, k)] acc_c(*acc, k) == acc_c(acc0, k) + sum(num_rt_seq_view(rows@.take(i as int)), |n: NumRow| rescan_w(rp, dp)(n) * ic(k)(n)),
            forall|k: GK| #![trigger acc_s(*acc, k)] acc_s(*acc, k) == acc_s(acc0, k) + sum(num_rt_seq_view(rows@.take(i as int)), |n: NumRow| rescan_w(rp, dp)(n) * iv(k)(n)),
            forall|k: GK| #![trigger acc_a(*acc, k)] acc_a(*acc, k) == acc_a(acc0, k) + sum(num_rt_seq_view(rows@.take(i as int)), |n: NumRow| rescan_w(rp, dp)(n) * ia(k)(n)),
        decreases rows.len() - i,
    {
        let r = &rows[i];
        let u2 = unmatched(c2, r);
        let u1 = unmatched(c1, r);
        let w: i64 = (if u2 { 1 } else { 0 }) - (if u1 { 1 } else { 0 });
        let ghost n = num_rt_view(*r);
        proof {
            lemma_wt(*c2, *c1, rp, dp, n);
            assert(w as int == rescan_w(rp, dp)(n));
        }
        let ghost before = *acc;
        emit_num(tab, acc, r, w);
        proof {
            let tk = rows@.take(i as int + 1);
            assert(tk =~= rows@.take(i as int).push(rows@[i as int]));
            let vs = num_rt_seq_view(rows@.take(i as int));
            assert(num_rt_seq_view(tk) =~= vs.push(n));
            assert forall|k: GK| #![trigger acc_c(*acc, k)] acc_c(*acc, k) == acc_c(acc0, k) + sum(num_rt_seq_view(tk), |n: NumRow| rescan_w(rp, dp)(n) * ic(k)(n)) by {
                lemma_sum_push(vs, n, |n: NumRow| rescan_w(rp, dp)(n) * ic(k)(n));
            }
            assert forall|k: GK| #![trigger acc_s(*acc, k)] acc_s(*acc, k) == acc_s(acc0, k) + sum(num_rt_seq_view(tk), |n: NumRow| rescan_w(rp, dp)(n) * iv(k)(n)) by {
                lemma_sum_push(vs, n, |n: NumRow| rescan_w(rp, dp)(n) * iv(k)(n));
            }
            assert forall|k: GK| #![trigger acc_a(*acc, k)] acc_a(*acc, k) == acc_a(acc0, k) + sum(num_rt_seq_view(tk), |n: NumRow| rescan_w(rp, dp)(n) * ia(k)(n)) by {
                lemma_sum_push(vs, n, |n: NumRow| rescan_w(rp, dp)(n) * ia(k)(n));
            }
        }
        i += 1;
    }
    proof { assert(rows@.take(i as int) =~= rows@); }
}

/// Changed num rows: their sign, if they had no match before.
pub fn acc_changed(tab: &Pow2Tab, acc: &mut Index<AccEnt>, dn: &Vec<NumDelta>, c2: &CMap, c1: &CMap, Ghost(rp): Ghost<Seq<PreRow>>, Ghost(dp): Ghost<Seq<(PreRow, int)>>)
    requires
        tab.ok(), acc_ok(*old(acc)), counts_ok(*c2, *c1, rp, dp),
        forall|i: int| 0 <= i < dn.len() ==> (#[trigger] dn@[i]).sign == 1 || dn@[i].sign == -1,
    ensures
        acc_ok(*final(acc)),
        forall|k: GK| #![trigger acc_c(*final(acc), k)] acc_c(*final(acc), k) == acc_c(*old(acc), k) + wsum(num_delta_seq_view(dn@), |n: NumRow| z1(rp, dp, n) * ic(k)(n)),
        forall|k: GK| #![trigger acc_s(*final(acc), k)] acc_s(*final(acc), k) == acc_s(*old(acc), k) + wsum(num_delta_seq_view(dn@), |n: NumRow| z1(rp, dp, n) * iv(k)(n)),
        forall|k: GK| #![trigger acc_a(*final(acc), k)] acc_a(*final(acc), k) == acc_a(*old(acc), k) + wsum(num_delta_seq_view(dn@), |n: NumRow| z1(rp, dp, n) * ia(k)(n)),
{
    let ghost acc0 = *acc;
    let mut i = 0;
    proof {
        assert(num_delta_seq_view(dn@.take(0)) =~= Seq::<(NumRow, int)>::empty());
        assert forall|k: GK| #![trigger acc_c(*acc, k)] acc_c(*acc, k) == acc_c(acc0, k) + wsum(num_delta_seq_view(dn@.take(0)), |n: NumRow| z1(rp, dp, n) * ic(k)(n)) by {
            lemma_sum_empty(Seq::<(NumRow, int)>::empty(), |x: (NumRow, int)| x.1 * (|n: NumRow| z1(rp, dp, n) * ic(k)(n))(x.0));
        }
        assert forall|k: GK| #![trigger acc_s(*acc, k)] acc_s(*acc, k) == acc_s(acc0, k) + wsum(num_delta_seq_view(dn@.take(0)), |n: NumRow| z1(rp, dp, n) * iv(k)(n)) by {
            lemma_sum_empty(Seq::<(NumRow, int)>::empty(), |x: (NumRow, int)| x.1 * (|n: NumRow| z1(rp, dp, n) * iv(k)(n))(x.0));
        }
        assert forall|k: GK| #![trigger acc_a(*acc, k)] acc_a(*acc, k) == acc_a(acc0, k) + wsum(num_delta_seq_view(dn@.take(0)), |n: NumRow| z1(rp, dp, n) * ia(k)(n)) by {
            lemma_sum_empty(Seq::<(NumRow, int)>::empty(), |x: (NumRow, int)| x.1 * (|n: NumRow| z1(rp, dp, n) * ia(k)(n))(x.0));
        }
    }
    while i < dn.len()
        invariant
            i <= dn.len(), tab.ok(), acc_ok(*acc), counts_ok(*c2, *c1, rp, dp),
            forall|i: int| 0 <= i < dn.len() ==> (#[trigger] dn@[i]).sign == 1 || dn@[i].sign == -1,
            forall|k: GK| #![trigger acc_c(*acc, k)] acc_c(*acc, k) == acc_c(acc0, k) + wsum(num_delta_seq_view(dn@.take(i as int)), |n: NumRow| z1(rp, dp, n) * ic(k)(n)),
            forall|k: GK| #![trigger acc_s(*acc, k)] acc_s(*acc, k) == acc_s(acc0, k) + wsum(num_delta_seq_view(dn@.take(i as int)), |n: NumRow| z1(rp, dp, n) * iv(k)(n)),
            forall|k: GK| #![trigger acc_a(*acc, k)] acc_a(*acc, k) == acc_a(acc0, k) + wsum(num_delta_seq_view(dn@.take(i as int)), |n: NumRow| z1(rp, dp, n) * ia(k)(n)),
        decreases dn.len() - i,
    {
        let d = &dn[i];
        let u1 = unmatched(c1, &d.row);
        let w: i64 = if u1 { d.sign } else { 0 };
        let ghost n = num_rt_view(d.row);
        proof {
            lemma_wt(*c2, *c1, rp, dp, n);
            assert(w as int == d.sign * z1(rp, dp, n));
        }
        emit_num(tab, acc, &d.row, w);
        proof {
            let tk = dn@.take(i as int + 1);
            assert(tk =~= dn@.take(i as int).push(dn@[i as int]));
            let vs = num_delta_seq_view(dn@.take(i as int));
            assert(num_delta_seq_view(tk) =~= vs.push((n, d.sign as int)));
            assert forall|k: GK| #![trigger acc_c(*acc, k)] acc_c(*acc, k) == acc_c(acc0, k) + wsum(num_delta_seq_view(tk), |n: NumRow| z1(rp, dp, n) * ic(k)(n)) by {
                lemma_sum_push(vs, (n, d.sign as int), |x: (NumRow, int)| x.1 * (|n: NumRow| z1(rp, dp, n) * ic(k)(n))(x.0));
                assert((d.sign as int) * (z1(rp, dp, n) * ic(k)(n)) == (d.sign * z1(rp, dp, n)) * ic(k)(n)) by (nonlinear_arith);
            }
            assert forall|k: GK| #![trigger acc_s(*acc, k)] acc_s(*acc, k) == acc_s(acc0, k) + wsum(num_delta_seq_view(tk), |n: NumRow| z1(rp, dp, n) * iv(k)(n)) by {
                lemma_sum_push(vs, (n, d.sign as int), |x: (NumRow, int)| x.1 * (|n: NumRow| z1(rp, dp, n) * iv(k)(n))(x.0));
                assert((d.sign as int) * (z1(rp, dp, n) * iv(k)(n)) == (d.sign * z1(rp, dp, n)) * iv(k)(n)) by (nonlinear_arith);
            }
            assert forall|k: GK| #![trigger acc_a(*acc, k)] acc_a(*acc, k) == acc_a(acc0, k) + wsum(num_delta_seq_view(tk), |n: NumRow| z1(rp, dp, n) * ia(k)(n)) by {
                lemma_sum_push(vs, (n, d.sign as int), |x: (NumRow, int)| x.1 * (|n: NumRow| z1(rp, dp, n) * ia(k)(n))(x.0));
                assert((d.sign as int) * (z1(rp, dp, n) * ia(k)(n)) == (d.sign * z1(rp, dp, n)) * ia(k)(n)) by (nonlinear_arith);
            }
        }
        i += 1;
    }
    proof { assert(dn@.take(i as int) =~= dn@); }
}

/// Scanned num rows: those with no pre row.
pub fn acc_scan(tab: &Pow2Tab, acc: &mut Index<AccEnt>, rows: &Vec<NumRt>, c: &CMap, Ghost(rp): Ghost<Seq<PreRow>>)
    requires
        tab.ok(), acc_ok(*old(acc)), c.wf(),
        forall|a: Seq<u8>, t: Seq<u8>, v: Seq<u8>| #[trigger] cm_at(*c, a, t, v) == pc(rp, a, t, v),
    ensures
        acc_ok(*final(acc)),
        forall|k: GK| #![trigger acc_c(*final(acc), k)] acc_c(*final(acc), k) == acc_c(*old(acc), k) + sum(num_rt_seq_view(rows@), |n: NumRow| z2(rp, n) * ic(k)(n)),
        forall|k: GK| #![trigger acc_s(*final(acc), k)] acc_s(*final(acc), k) == acc_s(*old(acc), k) + sum(num_rt_seq_view(rows@), |n: NumRow| z2(rp, n) * iv(k)(n)),
        forall|k: GK| #![trigger acc_a(*final(acc), k)] acc_a(*final(acc), k) == acc_a(*old(acc), k) + sum(num_rt_seq_view(rows@), |n: NumRow| z2(rp, n) * ia(k)(n)),
{
    let ghost acc0 = *acc;
    let mut i = 0;
    proof {
        assert forall|k: GK| #![trigger acc_c(*acc, k)] acc_c(*acc, k) == acc_c(acc0, k) + sum(num_rt_seq_view(rows@.take(0)), |n: NumRow| z2(rp, n) * ic(k)(n)) by {
            assert(num_rt_seq_view(rows@.take(0)) =~= Seq::<NumRow>::empty());
            lemma_sum_empty(Seq::<NumRow>::empty(), |n: NumRow| z2(rp, n) * ic(k)(n));
        }
        assert forall|k: GK| #![trigger acc_s(*acc, k)] acc_s(*acc, k) == acc_s(acc0, k) + sum(num_rt_seq_view(rows@.take(0)), |n: NumRow| z2(rp, n) * iv(k)(n)) by {
            assert(num_rt_seq_view(rows@.take(0)) =~= Seq::<NumRow>::empty());
            lemma_sum_empty(Seq::<NumRow>::empty(), |n: NumRow| z2(rp, n) * iv(k)(n));
        }
        assert forall|k: GK| #![trigger acc_a(*acc, k)] acc_a(*acc, k) == acc_a(acc0, k) + sum(num_rt_seq_view(rows@.take(0)), |n: NumRow| z2(rp, n) * ia(k)(n)) by {
            assert(num_rt_seq_view(rows@.take(0)) =~= Seq::<NumRow>::empty());
            lemma_sum_empty(Seq::<NumRow>::empty(), |n: NumRow| z2(rp, n) * ia(k)(n));
        }
    }
    while i < rows.len()
        invariant
            i <= rows.len(), tab.ok(), acc_ok(*acc), c.wf(),
            forall|a: Seq<u8>, t: Seq<u8>, v: Seq<u8>| #[trigger] cm_at(*c, a, t, v) == pc(rp, a, t, v),
            forall|k: GK| #![trigger acc_c(*acc, k)] acc_c(*acc, k) == acc_c(acc0, k) + sum(num_rt_seq_view(rows@.take(i as int)), |n: NumRow| z2(rp, n) * ic(k)(n)),
            forall|k: GK| #![trigger acc_s(*acc, k)] acc_s(*acc, k) == acc_s(acc0, k) + sum(num_rt_seq_view(rows@.take(i as int)), |n: NumRow| z2(rp, n) * iv(k)(n)),
            forall|k: GK| #![trigger acc_a(*acc, k)] acc_a(*acc, k) == acc_a(acc0, k) + sum(num_rt_seq_view(rows@.take(i as int)), |n: NumRow| z2(rp, n) * ia(k)(n)),
        decreases rows.len() - i,
    {
        let r = &rows[i];
        let u = unmatched(c, r);
        let w: i64 = if u { 1 } else { 0 };
        let ghost n = num_rt_view(*r);
        proof {
            lemma_wt_build(*c, rp, n);
            assert(w as int == z2(rp, n));
        }
        emit_num(tab, acc, r, w);
        proof {
            let tk = rows@.take(i as int + 1);
            assert(tk =~= rows@.take(i as int).push(rows@[i as int]));
            let vs = num_rt_seq_view(rows@.take(i as int));
            assert(num_rt_seq_view(tk) =~= vs.push(n));
            assert forall|k: GK| #![trigger acc_c(*acc, k)] acc_c(*acc, k) == acc_c(acc0, k) + sum(num_rt_seq_view(tk), |n: NumRow| z2(rp, n) * ic(k)(n)) by {
                lemma_sum_push(vs, n, |n: NumRow| z2(rp, n) * ic(k)(n));
            }
            assert forall|k: GK| #![trigger acc_s(*acc, k)] acc_s(*acc, k) == acc_s(acc0, k) + sum(num_rt_seq_view(tk), |n: NumRow| z2(rp, n) * iv(k)(n)) by {
                lemma_sum_push(vs, n, |n: NumRow| z2(rp, n) * iv(k)(n));
            }
            assert forall|k: GK| #![trigger acc_a(*acc, k)] acc_a(*acc, k) == acc_a(acc0, k) + sum(num_rt_seq_view(tk), |n: NumRow| z2(rp, n) * ia(k)(n)) by {
                lemma_sum_push(vs, n, |n: NumRow| z2(rp, n) * ia(k)(n));
            }
        }
        i += 1;
    }
    proof { assert(rows@.take(i as int) =~= rows@); }
}

} // verus!

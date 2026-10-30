// The signed join of three row lists into per-group contributions.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::*;
use crate::esc::*;
use crate::model::*;
use crate::algebra::*;
use crate::accmap::*;
use crate::buckets::*;
use crate::wideval::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;

verus! {

/// The join conditions an index lookup already established.
pub open spec fn jk_base(n: NumRow, s: SubRow, p: PreRow) -> bool {
    &&& n.adsh == Some(s.adsh)
    &&& p.adsh == n.adsh
    &&& n.tag is Some
    &&& p.tag == n.tag
    &&& n.version is Some
    &&& p.version == n.version
    &&& n.value is Some
    &&& n.uom is Some
}

pub open spec fn jk_full(n: NumRow, s: SubRow, p: PreRow) -> bool {
    jk_base(n, s, p) && s.fy is Some && p.stmt is Some
}

proof fn lemma_jk_iff(n: NumRow, s: SubRow, p: PreRow)
    ensures jk(n, s, p) <==> jk_full(n, s, p),
{}

/// w times the triple's contribution to group k.
pub fn emit_one(tab: &Pow2Tab, acc: &mut Index<AccEnt>, n: &NumRt, s: &SubRt, p: &PreRt, w: i64)
    requires
        tab.ok(),
        acc_ok(*old(acc)),
        w == 1 || w == -1,
        jk_base(num_rt_view(*n), sub_rt_view(*s), pre_rt_view(*p)),
    ensures
        acc_ok(*final(acc)),
        forall|k: GK| #![trigger acc_c(*final(acc), k)]
            acc_c(*final(acc), k) == acc_c(*old(acc), k) + w * icnt(k)(num_rt_view(*n), sub_rt_view(*s), pre_rt_view(*p)),
        forall|k: GK, sel: bool| #![trigger acc_v(*final(acc), k, sel)]
            acc_v(*final(acc), k, sel) == acc_v(*old(acc), k, sel) + w * iv(k, sel)(num_rt_view(*n), sub_rt_view(*s), pre_rt_view(*p)),
{
    let ghost (nv, sv, pv) = (num_rt_view(*n), sub_rt_view(*s), pre_rt_view(*p));
    if s.fy__null != 0 || p.stmt__null != 0 {
        proof {
            assert forall|k: GK| #![trigger acc_c(*acc, k)]
                acc_c(*acc, k) == acc_c(*old(acc), k) + w * icnt(k)(nv, sv, pv) by {
                assert(!jk(nv, sv, pv));
                assert(icnt(k)(nv, sv, pv) == 0);
                assert(w * 0 == 0);
            }
            assert forall|k: GK, sel: bool| #![trigger acc_v(*acc, k, sel)]
                acc_v(*acc, k, sel) == acc_v(*old(acc), k, sel) + w * iv(k, sel)(nv, sv, pv) by {
                assert(!jk(nv, sv, pv));
                assert(iv(k, sel)(nv, sv, pv) == 0);
                assert(w * 0 == 0);
            }
        }
        return;
    }
    let dc = Wide::from_i128(w as i128);
    let wv = wide_of_bits(tab, n.value);
    let av = if wv.is_neg() { wv.neg() } else { wv.dup() };
    let da = if w == 1 { av } else { av.neg() };
    let ds = if w == 1 { wv } else { wv.neg() };
    let name: Option<Vec<u8>> = if s.name__null != 0 { None } else { Some(copy_bytes(s.name.as_slice())) };
    let plabel: Option<Vec<u8>> = if p.plabel__null != 0 { None } else { Some(copy_bytes(p.plabel.as_slice())) };
    acc_add(acc, s.fy, p.stmt.as_slice(), n.uom.as_slice(), &name, n.tag.as_slice(), &plabel, &dc, &ds, &da);
    proof {
        assert(jk(nv, sv, pv));
        let key = GK { fy: s.fy as int, stmt: p.stmt@, uom: n.uom@, name: ov(name), tag: n.tag@, plabel: ov(plabel) };
        assert(key == key_of(nv, sv, pv));
        assert(ds@ == w * f64_val(n.value));
        assert(da@ == w * abs(f64_val(n.value)));
        assert forall|k: GK| #![trigger acc_c(*acc, k)]
            acc_c(*acc, k) == acc_c(*old(acc), k) + w * icnt(k)(nv, sv, pv) by {
            if k == key { assert(icnt(k)(nv, sv, pv) == 1); } else { assert(icnt(k)(nv, sv, pv) == 0); }
        }
        assert forall|k: GK, sel: bool| #![trigger acc_v(*acc, k, sel)]
            acc_v(*acc, k, sel) == acc_v(*old(acc), k, sel) + w * iv(k, sel)(nv, sv, pv) by {
            if k == key {
                assert(iv(k, sel)(nv, sv, pv) == if sel { abs(nv.value->0) } else { nv.value->0 });
                assert(acc_v(*acc, key, sel) == acc_v(*old(acc), key, sel) + if sel { da@ } else { ds@ });
                assert(nv.value->0 == f64_val(n.value));
                assert(ds@ == w * nv.value->0 && da@ == w * abs(nv.value->0));
            } else { assert(iv(k, sel)(nv, sv, pv) == 0); }
        }
        assert(nv == num_rt_view(*n) && sv == sub_rt_view(*s) && pv == pre_rt_view(*p));
        assert(acc_ok(*acc));
        assert forall|k: GK| #![trigger acc_c(*acc, k)]
            acc_c(*acc, k) == acc_c(*old(acc), k) + w * icnt(k)(num_rt_view(*n), sub_rt_view(*s), pre_rt_view(*p)) by {}
        assert forall|k: GK, sel: bool| #![trigger acc_v(*acc, k, sel)]
            acc_v(*acc, k, sel) == acc_v(*old(acc), k, sel) + w * iv(k, sel)(num_rt_view(*n), sub_rt_view(*s), pre_rt_view(*p)) by {}
    }
}

pub open spec fn acc_delta(base: Index<AccEnt>, cur: Index<AccEnt>, dc: spec_fn(GK) -> int, ds: spec_fn(GK, bool) -> int) -> bool {
    &&& acc_ok(cur)
    &&& forall|k: GK| #![trigger acc_c(cur, k)] acc_c(cur, k) == acc_c(base, k) + dc(k)
    &&& forall|k: GK, sel: bool| #![trigger acc_v(cur, k, sel)] acc_v(cur, k, sel) == acc_v(base, k, sel) + ds(k, sel)
}

pub proof fn lemma_acc_delta_ext(a: Index<AccEnt>, b: Index<AccEnt>, dc: spec_fn(GK) -> int, ds: spec_fn(GK, bool) -> int, ec: spec_fn(GK) -> int, es: spec_fn(GK, bool) -> int)
    requires acc_delta(a, b, dc, ds), forall|k: GK| #[trigger] dc(k) == ec(k), forall|k: GK, sel: bool| #[trigger] ds(k, sel) == es(k, sel),
    ensures acc_delta(a, b, ec, es),
{}

pub proof fn lemma_acc_delta_zero(a: Index<AccEnt>, dc: spec_fn(GK) -> int, ds: spec_fn(GK, bool) -> int)
    requires acc_ok(a), forall|k: GK| #[trigger] dc(k) == 0, forall|k: GK, sel: bool| #[trigger] ds(k, sel) == 0,
    ensures acc_delta(a, a, dc, ds),
{}

pub proof fn lemma_pos_member<R>(l: Seq<(R, int)>, m: spec_fn(R) -> bool, i: int)
    requires 0 <= i < pos_where(l, m).len(),
    ensures 0 <= pos_where(l, m)[i] < l.len(), m(l[pos_where(l, m)[i]].0),
    decreases l.len(),
{
    lemma_pos_in_range(l, m);
    let t = l.drop_last();
    let p = pos_where(t, m);
    if i < p.len() {
        lemma_pos_member(t, m, i);
        assert(pos_where(l, m)[i] == p[i]);
        assert(l[p[i]] == t[p[i]]);
    } else {
        assert(pos_where(l, m)[i] == l.len() - 1);
    }
}


pub open spec fn pq_c(vp: Seq<(PreRow, int)>, qs: Seq<int>, n: NumRow, s: SubRow, m: int) -> spec_fn(GK) -> int {
    |k: GK| m * bsum(vp, qs, pf(icnt(k), n, s))
}

pub open spec fn pq_s(vp: Seq<(PreRow, int)>, qs: Seq<int>, n: NumRow, s: SubRow, m: int) -> spec_fn(GK, bool) -> int {
    |k: GK, sel: bool| m * bsum(vp, qs, pf(iv(k, sel), n, s))
}

pub proof fn lemma_bsum_push<R>(l: Seq<(R, int)>, pos: Seq<int>, t: int, f: spec_fn(R) -> int)
    requires 0 <= t < pos.len(), forall|i: int| 0 <= i < pos.len() ==> 0 <= #[trigger] pos[i] < l.len(),
    ensures bsum(l, pos.take(t + 1), f) == bsum(l, pos.take(t), f) + l[pos[t]].1 * f(l[pos[t]].0),
{
    let g = |j: int| l[j].1 * f(l[j].0);
    assert(pos.take(t + 1).drop_last() =~= pos.take(t));
    assert(pos.take(t + 1).last() == pos[t]);
    lemma_sum_push(pos.take(t), pos[t], g);
    assert(pos.take(t + 1) =~= pos.take(t).push(pos[t]));
}

/// The pre rows of a bucket joined with one (num, sub) pair.
pub fn emit_pre_bucket(tab: &Pow2Tab, acc: &mut Index<AccEnt>, n: &NumRt, s: &SubRt, lp: &Vec<PreDelta>, bp: &[usize], m: i64)
    requires
        tab.ok(),
        acc_ok(*old(acc)),
        m == 1 || m == -1,
        forall|i: int| 0 <= i < lp.len() ==> (#[trigger] lp@[i]).sign == 1 || lp@[i].sign == -1,
        forall|t: int| 0 <= t < bp.len() ==> (#[trigger] bp@[t]) < lp.len() && jk_base(num_rt_view(*n), sub_rt_view(*s), pre_rt_view(lp@[bp@[t] as int].row)),
    ensures
        acc_delta(*old(acc), *final(acc),
            pq_c(pre_delta_seq_view(lp@), bp@.map_values(|x: usize| x as int), num_rt_view(*n), sub_rt_view(*s), m as int),
            pq_s(pre_delta_seq_view(lp@), bp@.map_values(|x: usize| x as int), num_rt_view(*n), sub_rt_view(*s), m as int)),
{
    let ghost vp = pre_delta_seq_view(lp@);
    let ghost ints = bp@.map_values(|x: usize| x as int);
    let ghost (nv, sv) = (num_rt_view(*n), sub_rt_view(*s));
    let mut t = 0;
    proof {
        assert(ints.take(0) =~= Seq::<int>::empty());
        lemma_acc_delta_zero(*acc, pq_c(vp, ints.take(0), nv, sv, m as int), pq_s(vp, ints.take(0), nv, sv, m as int));
    }
    while t < bp.len()
        invariant
            t <= bp.len(),
            tab.ok(),
            m == 1 || m == -1,
            vp == pre_delta_seq_view(lp@),
            ints == bp@.map_values(|x: usize| x as int),
            nv == num_rt_view(*n), sv == sub_rt_view(*s),
            forall|i: int| 0 <= i < lp.len() ==> (#[trigger] lp@[i]).sign == 1 || lp@[i].sign == -1,
            forall|t: int| 0 <= t < bp.len() ==> (#[trigger] bp@[t]) < lp.len() && jk_base(num_rt_view(*n), sub_rt_view(*s), pre_rt_view(lp@[bp@[t] as int].row)),
            acc_delta(*old(acc), *acc, pq_c(vp, ints.take(t as int), nv, sv, m as int), pq_s(vp, ints.take(t as int), nv, sv, m as int)),
        decreases bp.len() - t,
    {
        let q = bp[t];
        let w = m * lp[q].sign;
        let ghost before = *acc;
        emit_one(tab, acc, n, s, &lp[q].row, w);
        proof {
            assert forall|i: int| 0 <= i < ints.len() implies 0 <= #[trigger] ints[i] < vp.len() by { assert(bp@[i] < lp.len()); }
            let pvq = pre_rt_view(lp@[q as int].row);
            assert(vp[q as int] == (pvq, lp@[q as int].sign as int));
            assert(ints[t as int] == q as int);
            assert forall|k: GK| #![trigger acc_c(*acc, k)]
                acc_c(*acc, k) == acc_c(*old(acc), k) + pq_c(vp, ints.take(t as int + 1), nv, sv, m as int)(k) by {
                lemma_bsum_push(vp, ints, t as int, pf(icnt(k), nv, sv));
                let a = bsum(vp, ints.take(t as int), pf(icnt(k), nv, sv));
                let x = icnt(k)(nv, sv, pvq);
                let sg = lp@[q as int].sign as int;
                assert(acc_c(*acc, k) == acc_c(before, k) + w * x);
                assert(w as int == (m as int) * sg);
                assert((m as int) * (a + sg * x) == (m as int) * a + ((m as int) * sg) * x) by (nonlinear_arith);
            }
            assert forall|k: GK, sel: bool| #![trigger acc_v(*acc, k, sel)]
                acc_v(*acc, k, sel) == acc_v(*old(acc), k, sel) + pq_s(vp, ints.take(t as int + 1), nv, sv, m as int)(k, sel) by {
                lemma_bsum_push(vp, ints, t as int, pf(iv(k, sel), nv, sv));
                let a = bsum(vp, ints.take(t as int), pf(iv(k, sel), nv, sv));
                let x = iv(k, sel)(nv, sv, pvq);
                let sg = lp@[q as int].sign as int;
                assert(acc_v(*acc, k, sel) == acc_v(before, k, sel) + w * x);
                assert(w as int == (m as int) * sg);
                assert((m as int) * (a + sg * x) == (m as int) * a + ((m as int) * sg) * x) by (nonlinear_arith);
            }
        }
        t += 1;
    }
    proof { assert(ints.take(t as int) =~= ints); }
}


pub open spec fn sj_c(vs: Seq<(SubRow, int)>, vp: Seq<(PreRow, int)>, bs: Seq<int>, n: NumRow, m: int) -> spec_fn(GK) -> int {
    |k: GK| m * bsum(vs, bs, fp(vp, icnt(k), n))
}

pub open spec fn sj_s(vs: Seq<(SubRow, int)>, vp: Seq<(PreRow, int)>, bs: Seq<int>, n: NumRow, m: int) -> spec_fn(GK, bool) -> int {
    |k: GK, sel: bool| m * bsum(vs, bs, fp(vp, iv(k, sel), n))
}

pub open spec fn ls_c(vs: Seq<(SubRow, int)>, vp: Seq<(PreRow, int)>, n: NumRow, m: int) -> spec_fn(GK) -> int {
    |k: GK| m * lvl_s(vs, vp, icnt(k), n)
}

pub open spec fn ls_s(vs: Seq<(SubRow, int)>, vp: Seq<(PreRow, int)>, n: NumRow, m: int) -> spec_fn(GK, bool) -> int {
    |k: GK, sel: bool| m * lvl_s(vs, vp, iv(k, sel), n)
}

pub open spec fn sub_key(a: Seq<u8>) -> KeyView {
    (0int, esc(a))
}

proof fn lemma_zero_wsum<R>(l: Seq<(R, int)>, f: spec_fn(R) -> int)
    requires forall|r: R| #[trigger] f(r) == 0,
    ensures wsum(l, f) == 0,
{
    assert forall|i: int| 0 <= i < l.len() implies (|x: (R, int)| x.1 * f(x.0))(#[trigger] l[i]) == 0 by {
        assert(l[i].1 * f(l[i].0) == 0) by (nonlinear_arith) requires f(l[i].0) == 0;
    }
    lemma_sum_zero(l, |x: (R, int)| x.1 * f(x.0));
}

proof fn lemma_lvl_zero(vs: Seq<(SubRow, int)>, vp: Seq<(PreRow, int)>, h: H3, n: NumRow)
    requires forall|s: SubRow, p: PreRow| #[trigger] h(n, s, p) == 0,
    ensures lvl_s(vs, vp, h, n) == 0,
{
    assert forall|s: SubRow| #[trigger] fp(vp, h, n)(s) == 0 by {
        assert forall|p: PreRow| #[trigger] pf(h, n, s)(p) == 0 by {}
        lemma_zero_wsum(vp, pf(h, n, s));
    }
    lemma_zero_wsum(vs, fp(vp, h, n));
}

/// A num row's join runs over the buckets of its filing and its (tag, version).
proof fn lemma_lvl_bucket(vs: Seq<(SubRow, int)>, vp: Seq<(PreRow, int)>, h: H3, n: NumRow, a: Seq<u8>, t: Seq<u8>, v: Seq<u8>)
    requires
        forall|s: SubRow, p: PreRow| #[trigger] h(n, s, p) != 0 ==> s.adsh == a && pre_bk(p) == Some(pre_key3(a, t, v)),
    ensures
        lvl_s(vs, vp, h, n) == bsum(vs, pos_where(vs, m_sub(sub_key(a))), fp(vp, h, n)),
        forall|s: SubRow| lvl_p(vp, h, n, s) == bsum(vp, pos_where(vp, m_pre(pre_key3(a, t, v))), pf(h, n, s)),
{
    assert forall|s: SubRow| #[trigger] lvl_p(vp, h, n, s) == bsum(vp, pos_where(vp, m_pre(pre_key3(a, t, v))), pf(h, n, s)) by {
        assert forall|p: PreRow| !#[trigger] m_pre(pre_key3(a, t, v))(p) ==> pf(h, n, s)(p) == 0 by {}
        lemma_bucket_sum(vp, m_pre(pre_key3(a, t, v)), pf(h, n, s));
    }
    assert forall|s: SubRow| !#[trigger] m_sub(sub_key(a))(s) ==> fp(vp, h, n)(s) == 0 by {
        if !m_sub(sub_key(a))(s) {
            assert(s.adsh != a) by { if s.adsh == a {} }
            assert forall|p: PreRow| #[trigger] pf(h, n, s)(p) == 0 by {}
            lemma_zero_wsum(vp, pf(h, n, s));
        }
    }
    lemma_bucket_sum(vs, m_sub(sub_key(a)), fp(vp, h, n));
}


pub open spec fn bs_pq(b: &[usize]) -> Seq<int> {
    b@.map_values(|x: usize| x as int)
}

/// One num row of the list against the indexed sub and pre lists.
pub fn process_num(tab: &Pow2Tab, acc: &mut Index<AccEnt>, row: &NumRt, ls: &Vec<SubDelta>, lp: &Vec<PreDelta>,
    sidx: &Index<Bucket>, pidx: &Index<Bucket>, m: i64)
    requires
        tab.ok(),
        acc_ok(*old(acc)),
        m == 1 || m == -1,
        forall|i: int| 0 <= i < ls.len() ==> (#[trigger] ls@[i]).sign == 1 || ls@[i].sign == -1,
        forall|i: int| 0 <= i < lp.len() ==> (#[trigger] lp@[i]).sign == 1 || lp@[i].sign == -1,
        buckets_ok(*sidx, sub_delta_seq_view(ls@), |kv: KeyView| m_sub(kv)),
        buckets_ok(*pidx, pre_delta_seq_view(lp@), |kv: KeyView| m_pre(kv)),
    ensures
        acc_delta(*old(acc), *final(acc),
            ls_c(sub_delta_seq_view(ls@), pre_delta_seq_view(lp@), num_rt_view(*row), m as int),
            ls_s(sub_delta_seq_view(ls@), pre_delta_seq_view(lp@), num_rt_view(*row), m as int)),
{
    let ghost vs = sub_delta_seq_view(ls@);
    let ghost vp = pre_delta_seq_view(lp@);
    let ghost nr = num_rt_view(*row);
    let r = row;
    proof { assert(nr == num_rt_view(*r)); }
    if r.adsh__null != 0 || r.tag__null != 0 || r.version__null != 0 || r.value__null != 0 || r.uom__null != 0 {
        proof {
            assert forall|k: GK| #[trigger] ls_c(vs, vp, nr, m as int)(k) == 0 by {
                assert forall|s: SubRow, p: PreRow| #[trigger] icnt(k)(nr, s, p) == 0 by {}
                lemma_lvl_zero(vs, vp, icnt(k), nr);
                assert((m as int) * 0 == 0);
            }
            assert forall|k: GK, sel: bool| #[trigger] ls_s(vs, vp, nr, m as int)(k, sel) == 0 by {
                assert forall|s: SubRow, p: PreRow| #[trigger] iv(k, sel)(nr, s, p) == 0 by {}
                lemma_lvl_zero(vs, vp, iv(k, sel), nr);
                assert((m as int) * 0 == 0);
            }
            lemma_acc_delta_zero(*acc, ls_c(vs, vp, nr, m as int), ls_s(vs, vp, nr, m as int));
        }
        return;
    }
    let ghost (a, t, v) = (r.adsh@, r.tag@, r.version@);
    let mut stext = Vec::new();
    append_esc(&mut stext, r.adsh.as_slice());
    let skey = Key::composite(0, stext);
    let mut ptext = Vec::new();
    append_esc(&mut ptext, r.adsh.as_slice());
    append_esc(&mut ptext, r.tag.as_slice());
    append_esc(&mut ptext, r.version.as_slice());
    let pkey = Key::composite(0, ptext);
    proof {
        assert(skey.view() == sub_key(a));
        assert(pkey.view() == pre_key3(a, t, v));
        assert forall|k: GK, sel: bool, s: SubRow, p: PreRow| (icnt(k)(nr, s, p) != 0 ==> s.adsh == a && pre_bk(p) == Some(pre_key3(a, t, v)))
            && (iv(k, sel)(nr, s, p) != 0 ==> s.adsh == a && pre_bk(p) == Some(pre_key3(a, t, v))) by {}
        assert forall|k: GK| #![trigger lvl_s(vs, vp, icnt(k), nr)] lvl_s(vs, vp, icnt(k), nr) == bsum(vs, pos_where(vs, m_sub(sub_key(a))), fp(vp, icnt(k), nr)) by {
            lemma_lvl_bucket(vs, vp, icnt(k), nr, a, t, v);
        }
        assert forall|k: GK, sel: bool| #![trigger lvl_s(vs, vp, iv(k, sel), nr)] lvl_s(vs, vp, iv(k, sel), nr) == bsum(vs, pos_where(vs, m_sub(sub_key(a))), fp(vp, iv(k, sel), nr)) by {
            lemma_lvl_bucket(vs, vp, iv(k, sel), nr, a, t, v);
        }
        assert forall|k: GK, s: SubRow| #![trigger lvl_p(vp, icnt(k), nr, s)] lvl_p(vp, icnt(k), nr, s) == bsum(vp, pos_where(vp, m_pre(pre_key3(a, t, v))), pf(icnt(k), nr, s)) by {
            lemma_lvl_bucket(vs, vp, icnt(k), nr, a, t, v);
        }
        assert forall|k: GK, sel: bool, s: SubRow| #![trigger lvl_p(vp, iv(k, sel), nr, s)] lvl_p(vp, iv(k, sel), nr, s) == bsum(vp, pos_where(vp, m_pre(pre_key3(a, t, v))), pf(iv(k, sel), nr, s)) by {
            lemma_lvl_bucket(vs, vp, iv(k, sel), nr, a, t, v);
        }
    }
    let sb = sidx.get(&skey);
    let pb = pidx.get(&pkey);
    let empty: Vec<usize> = Vec::new();
    let bsl: &[usize] = match &sb { Some(b) => b.pos.as_slice(), None => empty.as_slice() };
    let bpl: &[usize] = match &pb { Some(b) => b.pos.as_slice(), None => empty.as_slice() };
    let ghost bs = bsl@.map_values(|x: usize| x as int);
    let ghost bp = bpl@.map_values(|x: usize| x as int);
    proof {
        assert(bs == pos_where(vs, m_sub(sub_key(a))));
        assert(bp == pos_where(vp, m_pre(pre_key3(a, t, v))));
        lemma_pos_in_range(vs, m_sub(sub_key(a)));
        lemma_pos_in_range(vp, m_pre(pre_key3(a, t, v)));
    }
    let mut jj = 0;
    let ghost acc0 = *acc;
    proof {
        assert(bs.take(0) =~= Seq::<int>::empty());
        lemma_acc_delta_zero(*acc, sj_c(vs, vp, bs.take(0), nr, m as int), sj_s(vs, vp, bs.take(0), nr, m as int));
    }
    while jj < bsl.len()
        invariant
            jj <= bsl.len(),
            tab.ok(),
            m == 1 || m == -1,
            vs == sub_delta_seq_view(ls@), vp == pre_delta_seq_view(lp@), nr == num_rt_view(*row), *r == *row,
            bs == bsl@.map_values(|x: usize| x as int), bp == bpl@.map_values(|x: usize| x as int),
            bs == pos_where(vs, m_sub(sub_key(a))), bp == pos_where(vp, m_pre(pre_key3(a, t, v))),
            a == r.adsh@, t == r.tag@, v == r.version@,
            r.adsh__null == 0 && r.tag__null == 0 && r.version__null == 0 && r.value__null == 0 && r.uom__null == 0,
            forall|i: int| 0 <= i < ls.len() ==> (#[trigger] ls@[i]).sign == 1 || ls@[i].sign == -1,
            forall|i: int| 0 <= i < lp.len() ==> (#[trigger] lp@[i]).sign == 1 || lp@[i].sign == -1,
            forall|i: int| 0 <= i < bs.len() ==> 0 <= #[trigger] bs[i] < vs.len(),
            forall|i: int| 0 <= i < bp.len() ==> 0 <= #[trigger] bp[i] < vp.len(),
            forall|k: GK| #![trigger lvl_s(vs, vp, icnt(k), nr)] lvl_s(vs, vp, icnt(k), nr) == bsum(vs, bs, fp(vp, icnt(k), nr)),
            forall|k: GK, sel: bool| #![trigger lvl_s(vs, vp, iv(k, sel), nr)] lvl_s(vs, vp, iv(k, sel), nr) == bsum(vs, bs, fp(vp, iv(k, sel), nr)),
            forall|k: GK, s: SubRow| #![trigger lvl_p(vp, icnt(k), nr, s)] lvl_p(vp, icnt(k), nr, s) == bsum(vp, bp, pf(icnt(k), nr, s)),
            forall|k: GK, sel: bool, s: SubRow| #![trigger lvl_p(vp, iv(k, sel), nr, s)] lvl_p(vp, iv(k, sel), nr, s) == bsum(vp, bp, pf(iv(k, sel), nr, s)),
            acc_delta(acc0, *acc, sj_c(vs, vp, bs.take(jj as int), nr, m as int), sj_s(vs, vp, bs.take(jj as int), nr, m as int)),
        decreases bsl.len() - jj,
    {
        let j = bsl[jj];
        proof {
            assert(bs[jj as int] == j as int);
            assert(vs.len() == ls@.len());
            assert(0 <= j < ls.len());
            assert(ls@[j as int].sign == 1 || ls@[j as int].sign == -1);
        }
        let m2 = m * ls[j].sign;
        let ghost before = *acc;
        proof {
            assert(bs[jj as int] == j as int);
            assert(0 <= j < ls.len());
            let sj = sub_rt_view(ls@[j as int].row);
            assert(vs[j as int] == (sj, ls@[j as int].sign as int));
            lemma_pos_member(vs, m_sub(sub_key(a)), jj as int);
            assert(sub_bk(sj) == sub_key(a));
            assert(esc(sj.adsh) + Seq::<u8>::empty() =~= esc(sj.adsh));
            assert(esc(a) + Seq::<u8>::empty() =~= esc(a));
            lemma_esc_split(sj.adsh, Seq::empty(), a, Seq::empty());
            assert forall|t2: int| 0 <= t2 < bpl.len() implies (#[trigger] bpl@[t2]) < lp.len() && jk_base(nr, sj, pre_rt_view(lp@[bpl@[t2] as int].row)) by {
                lemma_pos_member(vp, m_pre(pre_key3(a, t, v)), t2);
                let q = bpl@[t2] as int;
                let pq_ = pre_rt_view(lp@[q].row);
                assert(vp[q] == (pq_, lp@[q].sign as int));
                assert(pre_bk(pq_) == Some(pre_key3(a, t, v)));
                assert(pq_.adsh is Some && pq_.tag is Some && pq_.version is Some);
                assert(pre_bk(pq_) == Some(pre_key3(pq_.adsh->0, pq_.tag->0, pq_.version->0)));
                lemma_key3_inj(pq_.adsh->0, pq_.tag->0, pq_.version->0, a, t, v);
                assert(r.adsh__null == 0);
                assert(nr == num_rt_view(*r));
                assert(num_rt_view(*r).adsh == Some(r.adsh@));
                assert(nr.adsh == Some(r.adsh@));
                assert(nr.tag == Some(r.tag@));
                assert(nr.version == Some(r.version@));
                assert(a == r.adsh@ && t == r.tag@ && v == r.version@);
                assert(nr.adsh == Some(sj.adsh));
                assert(pq_.adsh == nr.adsh && pq_.tag == nr.tag && pq_.version == nr.version);
                assert(bpl@[t2] < lp.len());
            }
        }
        emit_pre_bucket(tab, acc, r, &ls[j].row, lp, bpl, m2);
        proof {
            let sj = sub_rt_view(ls@[j as int].row);
            let sg = ls@[j as int].sign as int;
            assert(vs[j as int] == (sj, sg));
            assert(m2 as int == (m as int) * sg);
            assert forall|k: GK| #![trigger acc_c(*acc, k)]
                acc_c(*acc, k) == acc_c(acc0, k) + sj_c(vs, vp, bs.take(jj as int + 1), nr, m as int)(k) by {
                lemma_bsum_push(vs, bs, jj as int, fp(vp, icnt(k), nr));
                let a0 = bsum(vs, bs.take(jj as int), fp(vp, icnt(k), nr));
                let lp_ = lvl_p(vp, icnt(k), nr, sj);
                assert(pq_c(vp, bs_pq(bpl), nr, sj, m2 as int)(k) == (m2 as int) * bsum(vp, bp, pf(icnt(k), nr, sj)));
                assert(acc_c(*acc, k) == acc_c(before, k) + (m2 as int) * bsum(vp, bp, pf(icnt(k), nr, sj)));
                assert(bsum(vp, bp, pf(icnt(k), nr, sj)) == lp_);
                assert(fp(vp, icnt(k), nr)(sj) == lp_);
                assert((m as int) * (a0 + sg * lp_) == (m as int) * a0 + ((m as int) * sg) * lp_) by (nonlinear_arith);
            }
            assert forall|k: GK, sel: bool| #![trigger acc_v(*acc, k, sel)]
                acc_v(*acc, k, sel) == acc_v(acc0, k, sel) + sj_s(vs, vp, bs.take(jj as int + 1), nr, m as int)(k, sel) by {
                lemma_bsum_push(vs, bs, jj as int, fp(vp, iv(k, sel), nr));
                let a0 = bsum(vs, bs.take(jj as int), fp(vp, iv(k, sel), nr));
                let lp_ = lvl_p(vp, iv(k, sel), nr, sj);
                assert(pq_s(vp, bs_pq(bpl), nr, sj, m2 as int)(k, sel) == (m2 as int) * bsum(vp, bp, pf(iv(k, sel), nr, sj)));
                assert(acc_v(*acc, k, sel) == acc_v(before, k, sel) + (m2 as int) * bsum(vp, bp, pf(iv(k, sel), nr, sj)));
                assert(bsum(vp, bp, pf(iv(k, sel), nr, sj)) == lp_);
                assert(fp(vp, iv(k, sel), nr)(sj) == lp_);
                assert((m as int) * (a0 + sg * lp_) == (m as int) * a0 + ((m as int) * sg) * lp_) by (nonlinear_arith);
            }
        }
        jj += 1;
    }
    proof {
        assert(bs.take(jj as int) =~= bs);
        assert forall|k: GK| #[trigger] sj_c(vs, vp, bs, nr, m as int)(k) == ls_c(vs, vp, nr, m as int)(k) by {}
        assert forall|k: GK, sel: bool| #[trigger] sj_s(vs, vp, bs, nr, m as int)(k, sel) == ls_s(vs, vp, nr, m as int)(k, sel) by {}
        lemma_acc_delta_ext(acc0, *acc, sj_c(vs, vp, bs, nr, m as int), sj_s(vs, vp, bs, nr, m as int), ls_c(vs, vp, nr, m as int), ls_s(vs, vp, nr, m as int));
    }
}


pub open spec fn nn_c(vn: Seq<(NumRow, int)>, vs: Seq<(SubRow, int)>, vp: Seq<(PreRow, int)>, m: int) -> spec_fn(GK) -> int {
    |k: GK| m * wsum(vn, fs(vs, vp, icnt(k)))
}

pub open spec fn nn_s(vn: Seq<(NumRow, int)>, vs: Seq<(SubRow, int)>, vp: Seq<(PreRow, int)>, m: int) -> spec_fn(GK, bool) -> int {
    |k: GK, sel: bool| m * wsum(vn, fs(vs, vp, iv(k, sel)))
}

pub proof fn lemma_wsum_push<R>(l: Seq<(R, int)>, i: int, f: spec_fn(R) -> int)
    requires 0 <= i < l.len(),
    ensures wsum(l.take(i + 1), f) == wsum(l.take(i), f) + l[i].1 * f(l[i].0),
{
    assert(l.take(i + 1).drop_last() =~= l.take(i));
    assert(l.take(i + 1).last() == l[i]);
    lemma_sum_push(l.take(i), l[i], |x: (R, int)| x.1 * f(x.0));
    assert(l.take(i + 1) =~= l.take(i).push(l[i]));
}

/// The signed join of the three lists, scaled by mult, added to the contributions.
pub fn join_lists(tab: &Pow2Tab, acc: &mut Index<AccEnt>, ln: &Vec<NumDelta>, ls: &Vec<SubDelta>, lp: &Vec<PreDelta>, mult: i64)
    requires
        tab.ok(),
        acc_ok(*old(acc)),
        mult == 1 || mult == -1,
        forall|i: int| 0 <= i < ln.len() ==> (#[trigger] ln@[i]).sign == 1 || ln@[i].sign == -1,
        forall|i: int| 0 <= i < ls.len() ==> (#[trigger] ls@[i]).sign == 1 || ls@[i].sign == -1,
        forall|i: int| 0 <= i < lp.len() ==> (#[trigger] lp@[i]).sign == 1 || lp@[i].sign == -1,
    ensures
        acc_delta(*old(acc), *final(acc),
            nn_c(num_delta_seq_view(ln@), sub_delta_seq_view(ls@), pre_delta_seq_view(lp@), mult as int),
            nn_s(num_delta_seq_view(ln@), sub_delta_seq_view(ls@), pre_delta_seq_view(lp@), mult as int)),
{
    let ghost vn = num_delta_seq_view(ln@);
    let ghost vs = sub_delta_seq_view(ls@);
    let ghost vp = pre_delta_seq_view(lp@);
    let sidx = build_sub(ls);
    let pidx = build_pre(lp);
    let ghost acc0 = *acc;
    let mut i = 0;
    proof {
        assert(vn.take(0) =~= Seq::<(NumRow, int)>::empty());
        lemma_acc_delta_zero(*acc, nn_c(vn.take(0), vs, vp, mult as int), nn_s(vn.take(0), vs, vp, mult as int));
    }
    while i < ln.len()
        invariant
            i <= ln.len(),
            tab.ok(),
            mult == 1 || mult == -1,
            vn == num_delta_seq_view(ln@), vs == sub_delta_seq_view(ls@), vp == pre_delta_seq_view(lp@),
            forall|i: int| 0 <= i < ln.len() ==> (#[trigger] ln@[i]).sign == 1 || ln@[i].sign == -1,
            forall|i: int| 0 <= i < ls.len() ==> (#[trigger] ls@[i]).sign == 1 || ls@[i].sign == -1,
            forall|i: int| 0 <= i < lp.len() ==> (#[trigger] lp@[i]).sign == 1 || lp@[i].sign == -1,
            buckets_ok(sidx, vs, |kv: KeyView| m_sub(kv)),
            buckets_ok(pidx, vp, |kv: KeyView| m_pre(kv)),
            acc_delta(acc0, *acc, nn_c(vn.take(i as int), vs, vp, mult as int), nn_s(vn.take(i as int), vs, vp, mult as int)),
        decreases ln.len() - i,
    {
        proof { assert(ln@[i as int].sign == 1 || ln@[i as int].sign == -1); }
        let m = mult * ln[i].sign;
        let ghost before = *acc;
        process_num(tab, acc, &ln[i].row, ls, lp, &sidx, &pidx, m);
        proof {
            let nr = num_rt_view(ln@[i as int].row);
            let sg = ln@[i as int].sign as int;
            assert(vn[i as int] == (nr, sg));
            assert(m as int == (mult as int) * sg);
            assert forall|k: GK| #![trigger acc_c(*acc, k)]
                acc_c(*acc, k) == acc_c(acc0, k) + nn_c(vn.take(i as int + 1), vs, vp, mult as int)(k) by {
                lemma_wsum_push(vn, i as int, fs(vs, vp, icnt(k)));
                let a0 = wsum(vn.take(i as int), fs(vs, vp, icnt(k)));
                let x = lvl_s(vs, vp, icnt(k), nr);
                assert(fs(vs, vp, icnt(k))(nr) == x);
                assert((mult as int) * (a0 + sg * x) == (mult as int) * a0 + ((mult as int) * sg) * x) by (nonlinear_arith);
            }
            assert forall|k: GK, sel: bool| #![trigger acc_v(*acc, k, sel)]
                acc_v(*acc, k, sel) == acc_v(acc0, k, sel) + nn_s(vn.take(i as int + 1), vs, vp, mult as int)(k, sel) by {
                lemma_wsum_push(vn, i as int, fs(vs, vp, iv(k, sel)));
                let a0 = wsum(vn.take(i as int), fs(vs, vp, iv(k, sel)));
                let x = lvl_s(vs, vp, iv(k, sel), nr);
                assert(fs(vs, vp, iv(k, sel))(nr) == x);
                assert((mult as int) * (a0 + sg * x) == (mult as int) * a0 + ((mult as int) * sg) * x) by (nonlinear_arith);
            }
        }
        i += 1;
    }
    proof { assert(vn.take(i as int) =~= vn); }
}


/// The join of the scanned num rows (weight 1) with the sub and pre lists, added to the contributions.
pub fn join_scan(tab: &Pow2Tab, acc: &mut Index<AccEnt>, nums: &Vec<NumRt>, ls: &Vec<SubDelta>, lp: &Vec<PreDelta>)
    requires
        tab.ok(),
        acc_ok(*old(acc)),
        forall|i: int| 0 <= i < ls.len() ==> (#[trigger] ls@[i]).sign == 1 || ls@[i].sign == -1,
        forall|i: int| 0 <= i < lp.len() ==> (#[trigger] lp@[i]).sign == 1 || lp@[i].sign == -1,
    ensures
        acc_delta(*old(acc), *final(acc),
            nn_c(unit_l(num_rt_seq_view(nums@)), sub_delta_seq_view(ls@), pre_delta_seq_view(lp@), 1),
            nn_s(unit_l(num_rt_seq_view(nums@)), sub_delta_seq_view(ls@), pre_delta_seq_view(lp@), 1)),
{
    let ghost vn = unit_l(num_rt_seq_view(nums@));
    let ghost vs = sub_delta_seq_view(ls@);
    let ghost vp = pre_delta_seq_view(lp@);
    let sidx = build_sub(ls);
    let pidx = build_pre(lp);
    let ghost acc0 = *acc;
    let mut i = 0;
    proof {
        assert(vn.take(0) =~= Seq::<(NumRow, int)>::empty());
        lemma_acc_delta_zero(*acc, nn_c(vn.take(0), vs, vp, 1), nn_s(vn.take(0), vs, vp, 1));
    }
    while i < nums.len()
        invariant
            i <= nums.len(),
            tab.ok(),
            vn == unit_l(num_rt_seq_view(nums@)), vs == sub_delta_seq_view(ls@), vp == pre_delta_seq_view(lp@),
            forall|i: int| 0 <= i < ls.len() ==> (#[trigger] ls@[i]).sign == 1 || ls@[i].sign == -1,
            forall|i: int| 0 <= i < lp.len() ==> (#[trigger] lp@[i]).sign == 1 || lp@[i].sign == -1,
            buckets_ok(sidx, vs, |kv: KeyView| m_sub(kv)),
            buckets_ok(pidx, vp, |kv: KeyView| m_pre(kv)),
            acc_delta(acc0, *acc, nn_c(vn.take(i as int), vs, vp, 1), nn_s(vn.take(i as int), vs, vp, 1)),
        decreases nums.len() - i,
    {
        process_num(tab, acc, &nums[i], ls, lp, &sidx, &pidx, 1);
        proof {
            let nr = num_rt_view(nums@[i as int]);
            assert(vn[i as int] == (nr, 1int));
            assert forall|k: GK| #![trigger acc_c(*acc, k)]
                acc_c(*acc, k) == acc_c(acc0, k) + nn_c(vn.take(i as int + 1), vs, vp, 1)(k) by {
                lemma_wsum_push(vn, i as int, fs(vs, vp, icnt(k)));
                let a0 = wsum(vn.take(i as int), fs(vs, vp, icnt(k)));
                let x = lvl_s(vs, vp, icnt(k), nr);
                assert(fs(vs, vp, icnt(k))(nr) == x);
            }
            assert forall|k: GK, sel: bool| #![trigger acc_v(*acc, k, sel)]
                acc_v(*acc, k, sel) == acc_v(acc0, k, sel) + nn_s(vn.take(i as int + 1), vs, vp, 1)(k, sel) by {
                lemma_wsum_push(vn, i as int, fs(vs, vp, iv(k, sel)));
                let a0 = wsum(vn.take(i as int), fs(vs, vp, iv(k, sel)));
                let x = lvl_s(vs, vp, iv(k, sel), nr);
                assert(fs(vs, vp, iv(k, sel))(nr) == x);
            }
        }
        i += 1;
    }
    proof { assert(vn.take(i as int) =~= vn); }
}

} // verus!

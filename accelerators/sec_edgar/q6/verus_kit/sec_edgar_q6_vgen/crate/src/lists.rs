// Delta lists derived from a transaction: keys, weighted copies, negated changes.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::algebra::*;
use crate::accmap::*;
use crate::glue_proofs::*;

verus! {


pub open spec fn keys_n(d: Seq<NumDelta>) -> Seq<Seq<u8>>
    decreases d.len(),
{
    if d.len() == 0 { Seq::empty() } else {
        keys_n(d.drop_last()) + if d.last().row.adsh__null == 0 { seq![d.last().row.adsh@] } else { Seq::empty() }
    }
}

pub open spec fn keys_p(d: Seq<PreDelta>) -> Seq<Seq<u8>>
    decreases d.len(),
{
    if d.len() == 0 { Seq::empty() } else {
        keys_p(d.drop_last()) + if d.last().row.adsh__null == 0 { seq![d.last().row.adsh@] } else { Seq::empty() }
    }
}

pub open spec fn keys_s(d: Seq<SubDelta>) -> Seq<Seq<u8>>
    decreases d.len(),
{
    if d.len() == 0 { Seq::empty() } else { keys_s(d.drop_last()) + seq![d.last().row.adsh@] }
}

pub open spec fn all_keys(dn: Seq<NumDelta>, dp: Seq<PreDelta>, ds: Seq<SubDelta>) -> Seq<Seq<u8>> {
    keys_n(dn) + keys_p(dp) + keys_s(ds)
}

pub open spec fn pn() -> spec_fn(NumRow) -> bool { |row: NumRow| row.value is Some && row.adsh is Some }
pub open spec fn pp() -> spec_fn(PreRow) -> bool { |row: PreRow| row.adsh is Some }
pub open spec fn ps() -> spec_fn(SubRow) -> bool { |row: SubRow| true }

pub proof fn lemma_contains_left(a: Seq<Seq<u8>>, b: Seq<Seq<u8>>, x: Seq<u8>)
    requires a.contains(x),
    ensures (a + b).contains(x),
{
    let j = choose|j: int| 0 <= j < a.len() && a[j] == x;
    assert((a + b)[j] == x);
}

pub proof fn lemma_contains_right(a: Seq<Seq<u8>>, b: Seq<Seq<u8>>, x: Seq<u8>)
    requires b.contains(x),
    ensures (a + b).contains(x),
{
    let j = choose|j: int| 0 <= j < b.len() && b[j] == x;
    assert((a + b)[a.len() + j] == x);
}

pub proof fn lemma_cp<A>(a: Seq<A>, b: Seq<A>, x: A)
    ensures (a + b).push(x) =~= a + (b + seq![x]), a + b =~= a + (b + Seq::<A>::empty()),
{}

pub proof fn lemma_view_push(r0: Seq<Vec<u8>>, v: Vec<u8>)
    ensures seq_view_bytes(r0.push(v)) =~= seq_view_bytes(r0).push(v@),
{}

pub fn collect_keys(dn: &Vec<NumDelta>, dp: &Vec<PreDelta>, ds: &Vec<SubDelta>) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == all_keys(dn@, dp@, ds@),
{
    let mut r: Vec<Vec<u8>> = Vec::new();
    let mut i = 0;
    while i < dn.len()
        invariant i <= dn.len(), seq_view_bytes(r@) == keys_n(dn@.take(i as int)),
        decreases dn.len() - i,
    {
        let tk = Ghost(dn@.take(i as int + 1));
        proof {
            assert(tk@.drop_last() =~= dn@.take(i as int));
            assert(tk@.last() == dn@[i as int]);
            assert(keys_n(tk@) == keys_n(tk@.drop_last()) + if tk@.last().row.adsh__null == 0 { seq![tk@.last().row.adsh@] } else { Seq::empty() }) by { reveal_with_fuel(keys_n, 2); }
        }
        let ghost r0 = r@;
        if dn[i].row.adsh__null == 0 {
            let cp = copy_bytes(dn[i].row.adsh.as_slice());
            r.push(cp);
            proof { lemma_view_push(r0, cp); }
        }
        proof { assert(seq_view_bytes(r@) =~= keys_n(tk@)); }
        i += 1;
    }
    proof { assert(dn@.take(i as int) =~= dn@); }
    let mut j = 0;
    while j < dp.len()
        invariant j <= dp.len(), seq_view_bytes(r@) == keys_n(dn@) + keys_p(dp@.take(j as int)),
        decreases dp.len() - j,
    {
        let tk = Ghost(dp@.take(j as int + 1));
        proof {
            assert(tk@.drop_last() =~= dp@.take(j as int));
            assert(tk@.last() == dp@[j as int]);
            assert(keys_p(tk@) == keys_p(tk@.drop_last()) + if tk@.last().row.adsh__null == 0 { seq![tk@.last().row.adsh@] } else { Seq::empty() }) by { reveal_with_fuel(keys_p, 2); }
        }
        let ghost r0 = r@;
        if dp[j].row.adsh__null == 0 {
            let cp = copy_bytes(dp[j].row.adsh.as_slice());
            r.push(cp);
            proof { lemma_view_push(r0, cp); }
        }
        proof { assert(seq_view_bytes(r@) =~= keys_n(dn@) + keys_p(tk@)); }
        j += 1;
    }
    proof { assert(dp@.take(j as int) =~= dp@); }
    let mut t = 0;
    while t < ds.len()
        invariant t <= ds.len(), seq_view_bytes(r@) == keys_n(dn@) + keys_p(dp@) + keys_s(ds@.take(t as int)),
        decreases ds.len() - t,
    {
        let tk = Ghost(ds@.take(t as int + 1));
        proof {
            assert(tk@.drop_last() =~= ds@.take(t as int));
            assert(tk@.last() == ds@[t as int]);
            assert(keys_s(tk@) == keys_s(tk@.drop_last()) + seq![tk@.last().row.adsh@]) by { reveal_with_fuel(keys_s, 2); }
        }
        let ghost r0 = r@;
        let cp = copy_bytes(ds[t].row.adsh.as_slice());
        r.push(cp);
        proof {
            lemma_view_push(r0, cp);
            assert(seq_view_bytes(r@) =~= keys_n(dn@) + keys_p(dp@) + keys_s(tk@));
        }
        t += 1;
    }
    proof { assert(ds@.take(t as int) =~= ds@); }
    r
}

pub proof fn lemma_keys_cover(dn: Seq<NumDelta>, dp: Seq<PreDelta>, ds: Seq<SubDelta>)
    ensures
        forall|i: int| 0 <= i < dn.len() && (#[trigger] dn[i]).row.adsh__null == 0 ==> all_keys(dn, dp, ds).contains(dn[i].row.adsh@),
        forall|i: int| 0 <= i < dp.len() && (#[trigger] dp[i]).row.adsh__null == 0 ==> all_keys(dn, dp, ds).contains(dp[i].row.adsh@),
        forall|i: int| 0 <= i < ds.len() ==> all_keys(dn, dp, ds).contains((#[trigger] ds[i]).row.adsh@),
{
    lemma_keys_n_contains(dn);
    lemma_keys_p_contains(dp);
    lemma_keys_s_contains(ds);
    assert forall|i: int| 0 <= i < dn.len() && (#[trigger] dn[i]).row.adsh__null == 0 implies all_keys(dn, dp, ds).contains(dn[i].row.adsh@) by {
        lemma_contains_left(keys_n(dn), keys_p(dp), dn[i].row.adsh@);
        lemma_contains_left(keys_n(dn) + keys_p(dp), keys_s(ds), dn[i].row.adsh@);
    }
    assert forall|i: int| 0 <= i < dp.len() && (#[trigger] dp[i]).row.adsh__null == 0 implies all_keys(dn, dp, ds).contains(dp[i].row.adsh@) by {
        lemma_contains_right(keys_n(dn), keys_p(dp), dp[i].row.adsh@);
        lemma_contains_left(keys_n(dn) + keys_p(dp), keys_s(ds), dp[i].row.adsh@);
    }
    assert forall|i: int| 0 <= i < ds.len() implies all_keys(dn, dp, ds).contains((#[trigger] ds[i]).row.adsh@) by {
        lemma_contains_right(keys_n(dn) + keys_p(dp), keys_s(ds), ds[i].row.adsh@);
    }
}

proof fn lemma_keys_n_contains(d: Seq<NumDelta>)
    ensures forall|i: int| 0 <= i < d.len() && (#[trigger] d[i]).row.adsh__null == 0 ==> keys_n(d).contains(d[i].row.adsh@),
    decreases d.len(),
{
    if d.len() > 0 {
        lemma_keys_n_contains(d.drop_last());
        assert forall|i: int| 0 <= i < d.len() && (#[trigger] d[i]).row.adsh__null == 0 implies keys_n(d).contains(d[i].row.adsh@) by {
            if i < d.len() - 1 {
                assert(d.drop_last()[i] == d[i]);
                assert(keys_n(d.drop_last()).contains(d[i].row.adsh@));
                let j = choose|j: int| 0 <= j < keys_n(d.drop_last()).len() && keys_n(d.drop_last())[j] == d[i].row.adsh@;
                assert(keys_n(d)[j] == keys_n(d.drop_last())[j]);
            } else {
                assert(keys_n(d)[keys_n(d).len() - 1] == d[i].row.adsh@);
            }
        }
    }
}

proof fn lemma_keys_p_contains(d: Seq<PreDelta>)
    ensures forall|i: int| 0 <= i < d.len() && (#[trigger] d[i]).row.adsh__null == 0 ==> keys_p(d).contains(d[i].row.adsh@),
    decreases d.len(),
{
    if d.len() > 0 {
        lemma_keys_p_contains(d.drop_last());
        assert forall|i: int| 0 <= i < d.len() && (#[trigger] d[i]).row.adsh__null == 0 implies keys_p(d).contains(d[i].row.adsh@) by {
            if i < d.len() - 1 {
                assert(d.drop_last()[i] == d[i]);
                assert(keys_p(d.drop_last()).contains(d[i].row.adsh@));
                let j = choose|j: int| 0 <= j < keys_p(d.drop_last()).len() && keys_p(d.drop_last())[j] == d[i].row.adsh@;
                assert(keys_p(d)[j] == keys_p(d.drop_last())[j]);
            } else {
                assert(keys_p(d)[keys_p(d).len() - 1] == d[i].row.adsh@);
            }
        }
    }
}

proof fn lemma_keys_s_contains(d: Seq<SubDelta>)
    ensures forall|i: int| 0 <= i < d.len() ==> keys_s(d).contains((#[trigger] d[i]).row.adsh@),
    decreases d.len(),
{
    if d.len() > 0 {
        lemma_keys_s_contains(d.drop_last());
        assert forall|i: int| 0 <= i < d.len() implies keys_s(d).contains((#[trigger] d[i]).row.adsh@) by {
            if i < d.len() - 1 {
                assert(d.drop_last()[i] == d[i]);
                assert(keys_s(d.drop_last()).contains(d[i].row.adsh@));
                let j = choose|j: int| 0 <= j < keys_s(d.drop_last()).len() && keys_s(d.drop_last())[j] == d[i].row.adsh@;
                assert(keys_s(d)[j] == keys_s(d.drop_last())[j]);
            } else {
                assert(keys_s(d)[keys_s(d).len() - 1] == d[i].row.adsh@);
            }
        }
    }
}

pub fn clone_pre(r: &PreRt) -> (c: PreRt)
    ensures pre_rt_view(c) == pre_rt_view(*r),
{
    PreRt { adsh: copy_bytes(r.adsh.as_slice()), adsh__null: r.adsh__null, stmt: copy_bytes(r.stmt.as_slice()), stmt__null: r.stmt__null, tag: copy_bytes(r.tag.as_slice()), tag__null: r.tag__null, version: copy_bytes(r.version.as_slice()), version__null: r.version__null, plabel: copy_bytes(r.plabel.as_slice()), plabel__null: r.plabel__null }
}

/// The rows as a list with weight 1.
pub fn wrap_pre(rows: &Vec<PreRt>) -> (out: Vec<PreDelta>)
    ensures pre_delta_seq_view(out@) == unit_l(pre_rt_seq_view(rows@)), forall|i: int| 0 <= i < out.len() ==> (#[trigger] out@[i]).sign == 1,
{
    let mut out: Vec<PreDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), pre_delta_seq_view(out@) == unit_l(pre_rt_seq_view(rows@.take(i as int))), forall|j: int| 0 <= j < out.len() ==> (#[trigger] out@[j]).sign == 1,
        decreases rows.len() - i,
    {
        out.push(PreDelta { row: clone_pre(&rows[i]), sign: 1 });
        proof {
            assert(rows@.take(i as int + 1) =~= rows@.take(i as int).push(rows@[i as int]));
            let v1 = pre_rt_seq_view(rows@.take(i as int + 1));
            assert(v1 =~= pre_rt_seq_view(rows@.take(i as int)).push(pre_rt_view(rows@[i as int])));
            assert(unit_l(v1) =~= unit_l(pre_rt_seq_view(rows@.take(i as int))).push((pre_rt_view(rows@[i as int]), 1int)));
            assert(pre_delta_seq_view(out@) =~= unit_l(v1));
        }
        i += 1;
    }
    proof { assert(rows@.take(i as int) =~= rows@); }
    out
}

pub fn clone_sub(r: &SubRt) -> (c: SubRt)
    ensures sub_rt_view(c) == sub_rt_view(*r),
{
    SubRt { adsh: copy_bytes(r.adsh.as_slice()), name: copy_bytes(r.name.as_slice()), name__null: r.name__null, fy: r.fy, fy__null: r.fy__null }
}

/// The rows as a list with weight 1.
pub fn wrap_sub(rows: &Vec<SubRt>) -> (out: Vec<SubDelta>)
    ensures sub_delta_seq_view(out@) == unit_l(sub_rt_seq_view(rows@)), forall|i: int| 0 <= i < out.len() ==> (#[trigger] out@[i]).sign == 1,
{
    let mut out: Vec<SubDelta> = Vec::new();
    let mut i = 0;
    while i < rows.len()
        invariant i <= rows.len(), sub_delta_seq_view(out@) == unit_l(sub_rt_seq_view(rows@.take(i as int))), forall|j: int| 0 <= j < out.len() ==> (#[trigger] out@[j]).sign == 1,
        decreases rows.len() - i,
    {
        out.push(SubDelta { row: clone_sub(&rows[i]), sign: 1 });
        proof {
            assert(rows@.take(i as int + 1) =~= rows@.take(i as int).push(rows@[i as int]));
            let v1 = sub_rt_seq_view(rows@.take(i as int + 1));
            assert(v1 =~= sub_rt_seq_view(rows@.take(i as int)).push(sub_rt_view(rows@[i as int])));
            assert(unit_l(v1) =~= unit_l(sub_rt_seq_view(rows@.take(i as int))).push((sub_rt_view(rows@[i as int]), 1int)));
            assert(sub_delta_seq_view(out@) =~= unit_l(v1));
        }
        i += 1;
    }
    proof { assert(rows@.take(i as int) =~= rows@); }
    out
}

pub proof fn lemma_negf_ext<A>(d: Seq<(A, int)>, c1: spec_fn(A) -> bool, c2: spec_fn(A) -> bool)
    requires forall|i: int| 0 <= i < d.len() ==> c1((#[trigger] d[i]).0) == c2(d[i].0),
    ensures negf(d, c1) == negf(d, c2),
    decreases d.len(),
{
    if d.len() > 0 {
        let t = d.drop_last();
        assert forall|i: int| 0 <= i < t.len() implies c1((#[trigger] t[i]).0) == c2(t[i].0) by { assert(t[i] == d[i]); }
        lemma_negf_ext(t, c1, c2);
        assert(c1(d.last().0) == c2(d.last().0)) by { assert(d[d.len() - 1] == d.last()); }
    }
}


/// The rows as a list with weight 1, consuming them.
pub fn wrap_pre_owned(rows: Vec<PreRt>) -> (out: Vec<PreDelta>)
    ensures pre_delta_seq_view(out@) == unit_l(pre_rt_seq_view(rows@)), forall|i: int| 0 <= i < out.len() ==> (#[trigger] out@[i]).sign == 1,
{
    let mut out: Vec<PreDelta> = Vec::with_capacity(rows.len());
    for r in it: rows.into_iter()
        invariant
            out.len() == it.index(), it.seq() == rows@,
            forall|i: int| 0 <= i < out.len() ==> (#[trigger] out@[i]) == (PreDelta { row: rows@[i], sign: 1 }),
    {
        out.push(PreDelta { row: r, sign: 1 });
        proof { assert(out@[out.len() - 1].row == rows@[it.index() as int]); }
    }
    proof {
        assert(pre_delta_seq_view(out@) =~= unit_l(pre_rt_seq_view(rows@)));
    }
    out
}

/// The rows as a list with weight 1, consuming them.
pub fn wrap_sub_owned(rows: Vec<SubRt>) -> (out: Vec<SubDelta>)
    ensures sub_delta_seq_view(out@) == unit_l(sub_rt_seq_view(rows@)), forall|i: int| 0 <= i < out.len() ==> (#[trigger] out@[i]).sign == 1,
{
    let mut out: Vec<SubDelta> = Vec::with_capacity(rows.len());
    for r in it: rows.into_iter()
        invariant
            out.len() == it.index(), it.seq() == rows@,
            forall|i: int| 0 <= i < out.len() ==> (#[trigger] out@[i]) == (SubDelta { row: rows@[i], sign: 1 }),
    {
        out.push(SubDelta { row: r, sign: 1 });
        proof { assert(out@[out.len() - 1].row == rows@[it.index() as int]); }
    }
    proof {
        assert(sub_delta_seq_view(out@) =~= unit_l(sub_rt_seq_view(rows@)));
    }
    out
}

} // verus!

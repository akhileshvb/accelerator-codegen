// The filings a transaction touches: the adsh of its changed num and pre rows.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::accmap::*;

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

pub open spec fn all_keys(dn: Seq<NumDelta>, dp: Seq<PreDelta>) -> Seq<Seq<u8>> {
    keys_n(dn) + keys_p(dp)
}

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

pub fn collect_keys(dn: &Vec<NumDelta>, dp: &Vec<PreDelta>) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == all_keys(dn@, dp@),
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
    r
}

pub proof fn lemma_keys_cover(dn: Seq<NumDelta>, dp: Seq<PreDelta>)
    ensures
        forall|i: int| 0 <= i < dn.len() && (#[trigger] dn[i]).row.adsh__null == 0 ==> all_keys(dn, dp).contains(dn[i].row.adsh@),
        forall|i: int| 0 <= i < dp.len() && (#[trigger] dp[i]).row.adsh__null == 0 ==> all_keys(dn, dp).contains(dp[i].row.adsh@),
{
    lemma_keys_n_contains(dn);
    lemma_keys_p_contains(dp);
    assert forall|i: int| 0 <= i < dn.len() && (#[trigger] dn[i]).row.adsh__null == 0 implies all_keys(dn, dp).contains(dn[i].row.adsh@) by {
        lemma_contains_left(keys_n(dn), keys_p(dp), dn[i].row.adsh@);
    }
    assert forall|i: int| 0 <= i < dp.len() && (#[trigger] dp[i]).row.adsh__null == 0 implies all_keys(dn, dp).contains(dp[i].row.adsh@) by {
        lemma_contains_right(keys_n(dn), keys_p(dp), dp[i].row.adsh@);
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

} // verus!

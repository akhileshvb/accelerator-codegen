// What a unit's groups depend on: the rescanned filings of the unit (absolute), and nothing a transaction left alone (frame).
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::algebra::*;
use crate::glue_proofs::*;

verus! {

/// Equal weights on the database's own rows give equal group sums.
pub proof fn lemma_gdb_ext_mem(db: Db, h1: H3, h2: H3)
    requires forall|n: NumRow, s: SubRow, p: PreRow| db.num.contains(n) && db.sub.contains(s) && db.pre.contains(p) ==> h1(n, s, p) == h2(n, s, p),
    ensures gdb(db, h1) == gdb(db, h2),
{
    lemma_gdb2(db, h1);
    lemma_gdb2(db, h2);
    assert forall|i: int| 0 <= i < db.num.len() implies (|n: NumRow| s_lvl(db.sub, db.pre, h1, n))(#[trigger] db.num[i]) == (|n: NumRow| s_lvl(db.sub, db.pre, h2, n))(db.num[i]) by {
        let n = db.num[i];
        assert forall|j: int| 0 <= j < db.sub.len() implies (|s: SubRow| p_lvl(db.pre, h1, n, s))(#[trigger] db.sub[j]) == (|s: SubRow| p_lvl(db.pre, h2, n, s))(db.sub[j]) by {
            let s = db.sub[j];
            assert forall|q: int| 0 <= q < db.pre.len() implies (|p: PreRow| h1(n, s, p))(#[trigger] db.pre[q]) == (|p: PreRow| h2(n, s, p))(db.pre[q]) by {
                assert(db.num.contains(n) && db.sub.contains(s) && db.pre.contains(db.pre[q]));
            }
            lemma_sum_ext(db.pre, |p: PreRow| h1(n, s, p), |p: PreRow| h2(n, s, p));
        }
        lemma_sum_ext(db.sub, |s: SubRow| p_lvl(db.pre, h1, n, s), |s: SubRow| p_lvl(db.pre, h2, n, s));
    }
    lemma_sum_ext(db.num, |n: NumRow| s_lvl(db.sub, db.pre, h1, n), |n: NumRow| s_lvl(db.sub, db.pre, h2, n));
}

pub open spec fn in_unit(s: SubRow, fy: int, name: Option<Seq<u8>>) -> bool {
    s.fy == Some(fy) && s.name == name
}

/// A unit whose database filings are all among the rescanned keys has the rescanned join as its groups.
pub proof fn lemma_unit_abs(db2: Db, keys: Seq<Seq<u8>>, rn: Seq<NumRow>, rs: Seq<SubRow>, rp: Seq<PreRow>, k: GK, h: H3)
    requires
        restr(unit_l(rn), db2.num, cnum(keys)), restr(unit_l(rs), db2.sub, csub(keys)), restr(unit_l(rp), db2.pre, cpre(keys)),
        forall|s: SubRow| db2.sub.contains(s) && in_unit(s, k.fy, k.name) ==> keys.contains(s.adsh),
        h == icnt(k) || h == ival(k),
    ensures
        gs(unit_l(rn), unit_l(rs), unit_l(rp), h) == gdb(db2, h),
{
    let (cn, cs, cp) = (cnum(keys), csub(keys), cpre(keys));
    lemma_gs_restr(db2, unit_l(rn), unit_l(rs), unit_l(rp), cn, cs, cp, h);
    let ha = hw(cn, cs, cp, h);
    assert forall|n: NumRow, s: SubRow, p: PreRow| db2.num.contains(n) && db2.sub.contains(s) && db2.pre.contains(p) implies #[trigger] ha(n, s, p) == h(n, s, p) by {
        if h(n, s, p) != 0 {
            assert(jk(n, s, p) && key_of(n, s, p) == k);
            assert(in_unit(s, k.fy, k.name));
            assert(keys.contains(s.adsh));
            assert(n.value is Some && n.adsh == Some(s.adsh) && p.adsh == n.adsh);
            assert(cn(n) == 1 && cs(s) == 1 && cp(p) == 1);
            assert(ha(n, s, p) == h(n, s, p)) by (nonlinear_arith)
                requires ha(n, s, p) == cn(n) * (cs(s) * (cp(p) * h(n, s, p))), cn(n) == 1, cs(s) == 1, cp(p) == 1;
        } else {
            assert(ha(n, s, p) == 0) by (nonlinear_arith)
                requires ha(n, s, p) == cn(n) * (cs(s) * (cp(p) * h(n, s, p))), h(n, s, p) == 0;
        }
    }
    lemma_gdb_ext_mem(db2, ha, h);
}

/// Filings with a delta row (by adsh).
pub open spec fn in_keys_a(keys: Seq<Seq<u8>>, a: Seq<u8>) -> bool {
    keys.contains(a)
}

/// A unit none of whose filings (before or after) has a delta row keeps its groups.
pub proof fn lemma_frame(db: Db, db2: Db, dn: Seq<(NumRow, int)>, ds: Seq<(SubRow, int)>, dp: Seq<(PreRow, int)>, keys: Seq<Seq<u8>>, k: GK, h: H3)
    requires
        bag_step(db.num, db2.num, dn), bag_step(db.sub, db2.sub, ds), bag_step(db.pre, db2.pre, dp),
        forall|i: int| 0 <= i < dn.len() && (#[trigger] dn[i]).0.adsh is Some ==> keys.contains(dn[i].0.adsh->0),
        forall|i: int| 0 <= i < ds.len() ==> keys.contains((#[trigger] ds[i]).0.adsh),
        forall|i: int| 0 <= i < dp.len() && (#[trigger] dp[i]).0.adsh is Some ==> keys.contains(dp[i].0.adsh->0),
        forall|s: SubRow| (db.sub.contains(s) || db2.sub.contains(s)) && in_unit(s, k.fy, k.name) ==> !keys.contains(s.adsh),
        h == icnt(k) || h == ival(k),
    ensures
        gdb(db2, h) == gdb(db, h),
{
    let g2 = |n: NumRow, s: SubRow, p: PreRow| if keys.contains(s.adsh) { 0int } else { h(n, s, p) };
    assert forall|i: int, s: SubRow, p: PreRow| 0 <= i < dn.len() implies #[trigger] g2(dn[i].0, s, p) == 0 by {
        if !keys.contains(s.adsh) && h(dn[i].0, s, p) != 0 {
            assert(dn[i].0.adsh == Some(s.adsh));
            assert(keys.contains(dn[i].0.adsh->0));
        }
    }
    assert forall|i: int, n: NumRow, p: PreRow| 0 <= i < ds.len() implies #[trigger] g2(n, ds[i].0, p) == 0 by {
        assert(keys.contains(ds[i].0.adsh));
    }
    assert forall|i: int, n: NumRow, s: SubRow| 0 <= i < dp.len() implies #[trigger] g2(n, s, dp[i].0) == 0 by {
        if !keys.contains(s.adsh) && h(n, s, dp[i].0) != 0 {
            assert(dp[i].0.adsh == n.adsh && n.adsh == Some(s.adsh));
            assert(keys.contains(dp[i].0.adsh->0));
        }
    }
    lemma_untouched(db, db2, dn, ds, dp, g2);
    assert forall|n: NumRow, s: SubRow, p: PreRow| db.num.contains(n) && db.sub.contains(s) && db.pre.contains(p) implies #[trigger] g2(n, s, p) == h(n, s, p) by {
        if h(n, s, p) != 0 { assert(in_unit(s, k.fy, k.name)); }
    }
    assert forall|n: NumRow, s: SubRow, p: PreRow| db2.num.contains(n) && db2.sub.contains(s) && db2.pre.contains(p) implies #[trigger] g2(n, s, p) == h(n, s, p) by {
        if h(n, s, p) != 0 { assert(in_unit(s, k.fy, k.name)); }
    }
    lemma_gdb_ext_mem(db, g2, h);
    lemma_gdb_ext_mem(db2, g2, h);
}

} // verus!

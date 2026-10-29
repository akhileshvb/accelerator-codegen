// The first state: every unit of the scanned sub rows is recomputed from the scan.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::algebra::*;
use crate::units::*;
use crate::plan::*;
use crate::subs::*;
use crate::frame::*;
use crate::inv::*;
use crate::umaps::*;
use crate::accmap::*;
use crate::glue_proofs::*;

verus! {

pub open spec fn empty_db() -> Db {
    Db { num: Seq::empty(), pre: Seq::empty(), sub: Seq::empty() }
}

/// The units of the sub rows with a non-NULL fy.
pub fn units_of_subs(rs: &Vec<SubRt>) -> (u: Index<UnitRef>)
    ensures
        uset_ok(u),
        forall|fy: int, name: Option<Seq<u8>>| #![trigger u_has(u, fy, name)] u_has(u, fy, name) <==> exists|i: int| 0 <= i < rs.len() && rs_unit(rs@, i, fy, name),
{
    let mut u: Index<UnitRef> = Index::new();
    let mut t = 0;
    while t < rs.len()
        invariant
            t <= rs.len(), uset_ok(u),
            forall|fy: int, name: Option<Seq<u8>>| #![trigger u_has(u, fy, name)] u_has(u, fy, name) <==> exists|i: int| 0 <= i < t && rs_unit(rs@, i, fy, name),
        decreases rs.len() - t,
    {
        let ghost u0 = u;
        if rs[t].fy__null == 0 {
            let nm: Option<Vec<u8>> = if rs[t].name__null != 0 { None } else { Some(copy_bytes(rs[t].name.as_slice())) };
            uset_add(&mut u, rs[t].fy, &nm);
            proof { assert(ov(nm) == sub_rt_view(rs@[t as int]).name); }
        }
        proof {
            assert forall|fy: int, name: Option<Seq<u8>>| u_has(u, fy, name) <==> exists|i: int| 0 <= i < t + 1 && rs_unit(rs@, i, fy, name) by {
                if rs@[t as int].fy__null == 0 && fy == rs@[t as int].fy as int && name == sub_rt_view(rs@[t as int]).name { assert(rs_unit(rs@, t as int, fy, name)); }
                if exists|i: int| 0 <= i < t + 1 && rs_unit(rs@, i, fy, name) { let i = choose|i: int| 0 <= i < t + 1 && rs_unit(rs@, i, fy, name); if i < t { assert(exists|i2: int| 0 <= i2 < t && rs_unit(rs@, i2, fy, name)); } }
                if exists|i: int| 0 <= i < t && rs_unit(rs@, i, fy, name) { assert(u_has(u0, fy, name)); }
            }
        }
        t += 1;
    }
    u
}

/// A scan of the sub table holds its sub rows.
pub proof fn lemma_scan_sub_has(db: Db, subs: Seq<SubRt>, a: Seq<u8>, fy: int, name: Option<Seq<u8>>)
    requires scan_subs_ok(db, subs),
    ensures sub_has(db, a, fy, name) <==> exists|i: int| 0 <= i < subs.len() && crate::umaps::rs_unit(subs, i, fy, name) && (#[trigger] subs[i]).adsh@ == a,
{
    let rv = sub_rt_seq_view(subs);
    if sub_has(db, a, fy, name) {
        let q = choose|q: int| 0 <= q < db.sub.len() && db.sub[q].adsh == a && db.sub[q].fy == Some(fy) && db.sub[q].name == name;
        let s = db.sub[q];
        lemma_contains_iff_sum(db.sub, s);
        lemma_contains_iff_sum(rv, s);
        assert(sum(rv, eqv(s)) == sum(db.sub, eqv(s)));
        assert(rv.contains(s));
        let i = choose|i: int| 0 <= i < rv.len() && rv[i] == s;
        assert(rv[i] == sub_rt_view(subs[i]));
    }
    if exists|i: int| 0 <= i < subs.len() && crate::umaps::rs_unit(subs, i, fy, name) && subs[i].adsh@ == a {
        let i = choose|i: int| 0 <= i < subs.len() && crate::umaps::rs_unit(subs, i, fy, name) && subs[i].adsh@ == a;
        let s = sub_rt_view(subs[i]);
        assert(rv[i] == s);
        assert(rv.contains(s));
        lemma_contains_iff_sum(db.sub, s);
        lemma_contains_iff_sum(rv, s);
        assert(sum(rv, eqv(s)) == sum(db.sub, eqv(s)));
        let q = choose|q: int| 0 <= q < db.sub.len() && db.sub[q] == s;
    }
}

/// The empty database has no group.
pub proof fn lemma_empty_groups(k: GK)
    ensures cntk(empty_db(), k) == 0, sumk(empty_db(), k) == 0,
{
    let db0 = empty_db();
    lemma_gdb2(db0, icnt(k));
    lemma_gdb2(db0, ival(k));
    lemma_sum_empty(db0.num, |n: NumRow| s_lvl(db0.sub, db0.pre, icnt(k), n));
    lemma_sum_empty(db0.num, |n: NumRow| s_lvl(db0.sub, db0.pre, ival(k), n));
}

/// A unit none of whose sub rows is in the database has no group.
pub proof fn lemma_absent_unit(db: Db, k: GK)
    requires forall|s: SubRow| db.sub.contains(s) ==> !in_unit(s, k.fy, k.name),
    ensures cntk(db, k) == 0, sumk(db, k) == 0,
{
    let z = |n: NumRow, s: SubRow, p: PreRow| 0int;
    assert forall|n: NumRow, s: SubRow, p: PreRow| db.num.contains(n) && db.sub.contains(s) && db.pre.contains(p) implies icnt(k)(n, s, p) == z(n, s, p) by {
        assert(!in_unit(s, k.fy, k.name));
    }
    assert forall|n: NumRow, s: SubRow, p: PreRow| db.num.contains(n) && db.sub.contains(s) && db.pre.contains(p) implies ival(k)(n, s, p) == z(n, s, p) by {
        assert(!in_unit(s, k.fy, k.name));
    }
    lemma_gdb_ext_mem(db, icnt(k), z);
    lemma_gdb_ext_mem(db, ival(k), z);
    lemma_gdb2(db, z);
    assert forall|n: NumRow, s: SubRow| #[trigger] p_lvl(db.pre, z, n, s) == 0 by { lemma_sum_zero(db.pre, |p: PreRow| z(n, s, p)); }
    assert forall|n: NumRow| #[trigger] s_lvl(db.sub, db.pre, z, n) == 0 by { lemma_sum_zero(db.sub, |s: SubRow| p_lvl(db.pre, z, n, s)); }
    lemma_sum_zero(db.num, |n: NumRow| s_lvl(db.sub, db.pre, z, n));
}

} // verus!

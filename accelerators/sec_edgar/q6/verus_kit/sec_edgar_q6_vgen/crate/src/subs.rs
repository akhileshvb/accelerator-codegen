// Which sub rows a transaction can have changed, and what a rescan holds.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::algebra::*;
use crate::units::*;

verus! {

pub proof fn lemma_contains_iff_sum<A>(t: Seq<A>, v: A)
    ensures t.contains(v) <==> sum(t, eqv(v)) > 0,
{
    assert forall|i: int| 0 <= i < t.len() implies eqv(v)(#[trigger] t[i]) >= 0 by {}
    lemma_sum_nonneg(t, eqv(v));
    if t.contains(v) {
        let i = choose|i: int| 0 <= i < t.len() && t[i] == v;
        lemma_sum_pos(t, eqv(v), i);
    }
    if sum(t, eqv(v)) > 0 {
        let i = lemma_sum_nonzero_witness(t, eqv(v));
        assert(t[i] == v);
    }
}

/// A sub row of a filing without a delta row is in the database after exactly when it was before.
pub proof fn lemma_sub_same(db: Db, db2: Db, ds: Seq<(SubRow, int)>, keys: Seq<Seq<u8>>, s: SubRow)
    requires
        bag_step(db.sub, db2.sub, ds),
        forall|i: int| 0 <= i < ds.len() ==> keys.contains((#[trigger] ds[i]).0.adsh),
        !keys.contains(s.adsh),
    ensures db.sub.contains(s) <==> db2.sub.contains(s),
{
    let f = eqv(s);
    assert(sum(db2.sub, f) == sum(db.sub, f) + sum(ds, |d: (SubRow, int)| d.1 * f(d.0)));
    assert forall|i: int| 0 <= i < ds.len() implies (|d: (SubRow, int)| d.1 * f(d.0))(#[trigger] ds[i]) == 0 by {
        assert(ds[i].0 != s);
        assert(f(ds[i].0) == 0);
        assert(ds[i].1 * 0 == 0) by (nonlinear_arith);
    }
    lemma_sum_zero(ds, |d: (SubRow, int)| d.1 * f(d.0));
    lemma_contains_iff_sum(db.sub, s);
    lemma_contains_iff_sum(db2.sub, s);
}

/// A sub row that is new is among the sub deltas.
pub proof fn lemma_new_sub_in_delta(db: Db, db2: Db, ds: Seq<(SubRow, int)>, s: SubRow) -> (i: int)
    requires bag_step(db.sub, db2.sub, ds), db2.sub.contains(s), !db.sub.contains(s),
    ensures 0 <= i < ds.len(), ds[i].0 == s,
{
    let f = eqv(s);
    lemma_contains_iff_sum(db.sub, s);
    lemma_contains_iff_sum(db2.sub, s);
    assert(sum(db2.sub, f) == sum(db.sub, f) + sum(ds, |d: (SubRow, int)| d.1 * f(d.0)));
    assert forall|i: int| 0 <= i < db.sub.len() implies f(#[trigger] db.sub[i]) >= 0 by {}
    lemma_sum_nonneg(db.sub, f);
    let w = lemma_sum_nonzero_witness(ds, |d: (SubRow, int)| d.1 * f(d.0));
    assert(ds[w].0 == s);
    w
}

/// A rescan holds exactly the database's sub rows of the filings it was asked for.
pub proof fn lemma_restr_contains(db2: Db, rs: Seq<SubRow>, keys: Seq<Seq<u8>>, s: SubRow)
    requires restr(unit_l(rs), db2.sub, csub(keys)),
    ensures rs.contains(s) <==> (db2.sub.contains(s) && keys.contains(s.adsh)),
{
    let f = eqv(s);
    lemma_wsum_unit(rs, f);
    assert(wsum(unit_l(rs), f) == sum(db2.sub, |r: SubRow| csub(keys)(r) * f(r)));
    lemma_contains_iff_sum(rs, s);
    lemma_contains_iff_sum(db2.sub, s);
    if keys.contains(s.adsh) {
        assert forall|r: SubRow| #[trigger] (csub(keys)(r) * f(r)) == f(r) by {
            if r == s { assert(csub(keys)(r) == 1); assert(1 * f(r) == f(r)); } else { assert(f(r) == 0); assert(csub(keys)(r) * 0 == 0) by (nonlinear_arith); }
        }
        lemma_sum_ext_all(db2.sub, |r: SubRow| csub(keys)(r) * f(r), f);
    } else {
        assert forall|r: SubRow| #[trigger] (csub(keys)(r) * f(r)) == 0 by {
            if r == s { assert(csub(keys)(r) == 0); assert(0 * f(r) == 0) by (nonlinear_arith); } else { assert(f(r) == 0); assert(csub(keys)(r) * 0 == 0) by (nonlinear_arith); }
        }
        lemma_sum_zero(db2.sub, |r: SubRow| csub(keys)(r) * f(r));
    }
}

} // verus!

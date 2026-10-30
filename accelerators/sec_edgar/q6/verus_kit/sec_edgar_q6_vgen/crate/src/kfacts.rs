// Facts that tie a transaction, its rescan and the plan to the maps and the table.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::algebra::*;
use crate::units::*;
use crate::plan::*;
use crate::keys::*;
use crate::canon::*;
use crate::lists::*;
use crate::subs::*;
use crate::frame::*;
use crate::bridge::*;
use crate::inv::*;
use crate::umaps::*;

verus! {

/// The units outside the plan's touched set keep their groups.
pub proof fn lemma_frame_units(db: Db, db2: Db, dn: Seq<NumDelta>, dp: Seq<PreDelta>, ds: Seq<SubDelta>, fm: Index<Vec<UnitRef>>, u: Index<UnitRef>)
    requires
        capture_ok(db, db2, num_delta_seq_view(dn), pre_delta_seq_view(dp), sub_delta_seq_view(ds)),
        fm_ok(fm, db),
        forall|fy: int, name: Option<Seq<u8>>| #![trigger u_has(u, fy, name)] u_has(u, fy, name) <==> u_pred_s(fm, all_keys(dn, dp, ds), ds, fy, name),
    ensures frame_ok(db, db2, u),
{
    let kall = all_keys(dn, dp, ds);
    lemma_keys_cover(dn, dp, ds);
    let (vn, vp, vs) = (num_delta_seq_view(dn), pre_delta_seq_view(dp), sub_delta_seq_view(ds));
    assert forall|k: GK| !u_has(u, k.fy, k.name) implies cntk(db2, k) == cntk(db, k) && sumk(db2, k) == sumk(db, k) && absk(db2, k) == absk(db, k) by {
        assert forall|i: int| 0 <= i < vn.len() && (#[trigger] vn[i]).0.adsh is Some implies kall.contains(vn[i].0.adsh->0) by {
            assert(vn[i].0 == num_rt_view(dn[i].row));
            assert(dn[i].row.adsh__null == 0);
        }
        assert forall|i: int| 0 <= i < vp.len() && (#[trigger] vp[i]).0.adsh is Some implies kall.contains(vp[i].0.adsh->0) by {
            assert(vp[i].0 == pre_rt_view(dp[i].row));
            assert(dp[i].row.adsh__null == 0);
        }
        assert forall|i: int| 0 <= i < vs.len() implies kall.contains((#[trigger] vs[i]).0.adsh) by {
            assert(vs[i].0 == sub_rt_view(ds[i].row));
        }
        assert forall|s: SubRow| (db.sub.contains(s) || db2.sub.contains(s)) && in_unit(s, k.fy, k.name) implies !kall.contains(s.adsh) by {
            if kall.contains(s.adsh) {
                let i = choose|i: int| 0 <= i < kall.len() && kall[i] == s.adsh;
                if db.sub.contains(s) {
                    let q = choose|q: int| 0 <= q < db.sub.len() && db.sub[q] == s;
                    assert(sub_has(db, s.adsh, k.fy, k.name));
                    assert(fm_has(fm, kall[i], k.fy, k.name));
                    assert(u_pred_s(fm, kall, ds, k.fy, k.name));
                } else {
                    let w = lemma_new_sub_in_delta(db, db2, vs, s);
                    assert(vs[w].0 == sub_rt_view(ds[w].row));
                    assert(sd_at(ds, w, k.fy, k.name));
                    assert(sd_has(ds, k.fy, k.name));
                    assert(u_pred_s(fm, kall, ds, k.fy, k.name));
                }
            }
        }
        lemma_frame(db, db2, vn, vs, vp, kall, k, icnt(k));
        lemma_frame(db, db2, vn, vs, vp, kall, k, ival(k));
        lemma_frame(db, db2, vn, vs, vp, kall, k, iabs(k));
    }
}

/// A sub row of the new database of a filing the plan did not ask for was there before (so the unit map lists it).
pub proof fn lemma_unit_filings_asked(db: Db, db2: Db, dn: Seq<NumDelta>, dp: Seq<PreDelta>, ds: Seq<SubDelta>, cls: Index<ClassRec>, fm: Index<Vec<UnitRef>>, um: Index<UnitRec>,
    keys: Seq<Seq<u8>>, w: Index<UnitRef>, fy: int, name: Option<Seq<u8>>, a: Seq<u8>)
    requires
        capture_ok(db, db2, num_delta_seq_view(dn), pre_delta_seq_view(dp), sub_delta_seq_view(ds)),
        um_ok(um, db),
        canon_keys(keys, wanted_pred(cls, fm, um, all_keys(dn, dp, ds), ds)),
        forall|f: int, n: Option<Seq<u8>>| #![trigger u_has(w, f, n)] u_has(w, f, n) <==> w_pred(cls, fm, all_keys(dn, dp, ds), ds, f, n),
        u_has(w, fy, name), sub_has(db2, a, fy, name),
    ensures keys.contains(a),
{
    let kall = all_keys(dn, dp, ds);
    let vs = sub_delta_seq_view(ds);
    if !kall.contains(a) {
        let i = choose|i: int| 0 <= i < db2.sub.len() && db2.sub[i].adsh == a && db2.sub[i].fy == Some(fy) && db2.sub[i].name == name;
        let s = db2.sub[i];
        lemma_keys_cover(dn, dp, ds);
        assert forall|j: int| 0 <= j < vs.len() implies kall.contains((#[trigger] vs[j]).0.adsh) by { assert(vs[j].0 == sub_rt_view(ds[j].row)); }
        lemma_sub_same(db, db2, vs, kall, s);
        assert(db.sub.contains(s));
        let q = choose|q: int| 0 <= q < db.sub.len() && db.sub[q] == s;
        assert(sub_has(db, a, fy, name));
        assert(um_has(um, fy, name, a));
        assert(w_pred(cls, fm, kall, ds, fy, name));
        assert(wanted_pred(cls, fm, um, kall, ds)(a));
    } else {
        assert(wanted_pred(cls, fm, um, kall, ds)(a));
    }
}

pub proof fn lemma_sub_has_asked(db2: Db, rs: Seq<SubRt>, keys: Seq<Seq<u8>>, a: Seq<u8>, fy: int, name: Option<Seq<u8>>)
    requires restr(unit_l(sub_rt_seq_view(rs)), db2.sub, csub(keys)), keys.contains(a),
    ensures sub_has(db2, a, fy, name) <==> exists|i: int| 0 <= i < rs.len() && rs_unit(rs, i, fy, name) && (#[trigger] rs[i]).adsh@ == a,
{
    let rv = sub_rt_seq_view(rs);
    if sub_has(db2, a, fy, name) {
        let q = choose|q: int| 0 <= q < db2.sub.len() && db2.sub[q].adsh == a && db2.sub[q].fy == Some(fy) && db2.sub[q].name == name;
        let s = db2.sub[q];
        assert(db2.sub.contains(s));
        lemma_restr_contains(db2, rv, keys, s);
        assert(rv.contains(s));
        let i = choose|i: int| 0 <= i < rv.len() && rv[i] == s;
        assert(rv[i] == sub_rt_view(rs[i]));
    }
    if exists|i: int| 0 <= i < rs.len() && rs_unit(rs, i, fy, name) && rs[i].adsh@ == a {
        let i = choose|i: int| 0 <= i < rs.len() && rs_unit(rs, i, fy, name) && rs[i].adsh@ == a;
        let s = sub_rt_view(rs[i]);
        assert(rv[i] == s);
        assert(rv.contains(s));
        lemma_restr_contains(db2, rv, keys, s);
        let q = choose|q: int| 0 <= q < db2.sub.len() && db2.sub[q] == s;
        assert(db2.sub[q].adsh == a && db2.sub[q].fy == Some(fy) && db2.sub[q].name == name);
    }
}

pub proof fn lemma_rescan_in_keys(db2: Db, rs: Seq<SubRt>, keys: Seq<Seq<u8>>, i: int)
    requires restr(unit_l(sub_rt_seq_view(rs)), db2.sub, csub(keys)), 0 <= i < rs.len(),
    ensures keys.contains(rs[i].adsh@),
{
    let rv = sub_rt_seq_view(rs);
    let s = sub_rt_view(rs[i]);
    assert(rv[i] == s);
    assert(rv.contains(s));
    lemma_restr_contains(db2, rv, keys, s);
}

pub proof fn lemma_sub_has_unasked(db: Db, db2: Db, ds: Seq<(SubRow, int)>, keys: Seq<Seq<u8>>, a: Seq<u8>, fy: int, name: Option<Seq<u8>>)
    requires
        bag_step(db.sub, db2.sub, ds), forall|i: int| 0 <= i < ds.len() ==> keys.contains((#[trigger] ds[i]).0.adsh), !keys.contains(a),
    ensures sub_has(db, a, fy, name) <==> sub_has(db2, a, fy, name),
{
    if sub_has(db, a, fy, name) {
        let q = choose|q: int| 0 <= q < db.sub.len() && db.sub[q].adsh == a && db.sub[q].fy == Some(fy) && db.sub[q].name == name;
        lemma_sub_same(db, db2, ds, keys, db.sub[q]);
        let q2 = choose|q2: int| 0 <= q2 < db2.sub.len() && db2.sub[q2] == db.sub[q];
    }
    if sub_has(db2, a, fy, name) {
        let q = choose|q: int| 0 <= q < db2.sub.len() && db2.sub[q].adsh == a && db2.sub[q].fy == Some(fy) && db2.sub[q].name == name;
        lemma_sub_same(db, db2, ds, keys, db2.sub[q]);
        let q2 = choose|q2: int| 0 <= q2 < db.sub.len() && db.sub[q2] == db2.sub[q];
    }
}

pub proof fn lemma_fm_ok2(db: Db, db2: Db, ds: Seq<(SubRow, int)>, keys: Seq<Seq<u8>>, rs: Seq<SubRt>, fm: Index<Vec<UnitRef>>, fm2: Index<Vec<UnitRef>>, ks: Seq<Vec<u8>>)
    requires
        bag_step(db.sub, db2.sub, ds), forall|i: int| 0 <= i < ds.len() ==> keys.contains((#[trigger] ds[i]).0.adsh),
        restr(unit_l(sub_rt_seq_view(rs)), db2.sub, csub(keys)), fm_ok(fm, db), seq_view_bytes(ks) == keys, fm2.wf(),
        forall|a: Seq<u8>, fy: int, name: Option<Seq<u8>>| #![trigger fm_has(fm2, a, fy, name)] fm_has(fm2, a, fy, name) <==>
            ((!seq_view_bytes(ks).contains(a) && fm_has(fm, a, fy, name)) || exists|i: int| 0 <= i < rs.len() && (#[trigger] rs[i]).adsh@ == a && rs_unit(rs, i, fy, name)),
    ensures fm_ok(fm2, db2),
{
    assert forall|a: Seq<u8>, fy: int, name: Option<Seq<u8>>| fm_has(fm2, a, fy, name) <==> sub_has(db2, a, fy, name) by {
        if keys.contains(a) {
            lemma_sub_has_asked(db2, rs, keys, a, fy, name);
        } else {
            lemma_sub_has_unasked(db, db2, ds, keys, a, fy, name);
            if exists|i: int| 0 <= i < rs.len() && rs[i].adsh@ == a && rs_unit(rs, i, fy, name) {
                let i = choose|i: int| 0 <= i < rs.len() && rs[i].adsh@ == a && rs_unit(rs, i, fy, name);
                lemma_rescan_in_keys(db2, rs, keys, i);
            }
        }
    }
}

pub proof fn lemma_um_ok2(db: Db, db2: Db, ds: Seq<(SubRow, int)>, kall: Seq<Seq<u8>>, keys: Seq<Seq<u8>>, rs: Seq<SubRt>, um: Index<UnitRec>, um2: Index<UnitRec>, u: Index<UnitRef>)
    requires
        bag_step(db.sub, db2.sub, ds), forall|i: int| 0 <= i < ds.len() ==> kall.contains((#[trigger] ds[i]).0.adsh),
        restr(unit_l(sub_rt_seq_view(rs)), db2.sub, csub(keys)), um_ok(um, db), um2.wf(),
        // units with a sub row of a delta filing (before or after) are in u; the asked filings hold every filing of the units in u
        forall|fy: int, name: Option<Seq<u8>>, a: Seq<u8>| u_has(u, fy, name) && sub_has(db2, a, fy, name) ==> keys.contains(a),
        forall|fy: int, name: Option<Seq<u8>>, a: Seq<u8>| !u_has(u, fy, name) && (sub_has(db, a, fy, name) || sub_has(db2, a, fy, name)) ==> !kall.contains(a),
        forall|fy: int, name: Option<Seq<u8>>, a: Seq<u8>| #![trigger um_has(um2, fy, name, a)] um_has(um2, fy, name, a) <==>
            (if u_has(u, fy, name) { exists|i: int| 0 <= i < rs.len() && rs_unit(rs, i, fy, name) && (#[trigger] rs[i]).adsh@ == a } else { um_has(um, fy, name, a) }),
    ensures um_ok(um2, db2),
{
    assert forall|fy: int, name: Option<Seq<u8>>, a: Seq<u8>| um_has(um2, fy, name, a) <==> sub_has(db2, a, fy, name) by {
        if u_has(u, fy, name) {
            if sub_has(db2, a, fy, name) { assert(keys.contains(a)); lemma_sub_has_asked(db2, rs, keys, a, fy, name); }
            else if exists|i: int| 0 <= i < rs.len() && rs_unit(rs, i, fy, name) && rs[i].adsh@ == a {
                let i = choose|i: int| 0 <= i < rs.len() && rs_unit(rs, i, fy, name) && rs[i].adsh@ == a;
                lemma_rescan_in_keys(db2, rs, keys, i);
                lemma_sub_has_asked(db2, rs, keys, a, fy, name);
            }
        } else {
            assert(um_has(um, fy, name, a) == sub_has(db, a, fy, name));
            if sub_has(db, a, fy, name) || sub_has(db2, a, fy, name) { assert(!kall.contains(a)); lemma_sub_has_unasked(db, db2, ds, kall, a, fy, name); }
        }
    }
}

/// A unit outside the plan's set has no sub row, before or after, of a filing with a delta row.
pub proof fn lemma_not_u_not_asked(db: Db, db2: Db, dn: Seq<NumDelta>, dp: Seq<PreDelta>, ds: Seq<SubDelta>, fm: Index<Vec<UnitRef>>, u: Index<UnitRef>, fy: int, name: Option<Seq<u8>>, a: Seq<u8>)
    requires
        capture_ok(db, db2, num_delta_seq_view(dn), pre_delta_seq_view(dp), sub_delta_seq_view(ds)),
        fm_ok(fm, db),
        forall|f: int, n: Option<Seq<u8>>| #![trigger u_has(u, f, n)] u_has(u, f, n) <==> u_pred_s(fm, all_keys(dn, dp, ds), ds, f, n),
        !u_has(u, fy, name), sub_has(db, a, fy, name) || sub_has(db2, a, fy, name),
    ensures !all_keys(dn, dp, ds).contains(a),
{
    let kall = all_keys(dn, dp, ds);
    let vs = sub_delta_seq_view(ds);
    if kall.contains(a) {
        let i = choose|i: int| 0 <= i < kall.len() && kall[i] == a;
        if sub_has(db, a, fy, name) {
            assert(fm_has(fm, kall[i], fy, name));
            assert(u_pred_s(fm, kall, ds, fy, name));
        } else {
            let q = choose|q: int| 0 <= q < db2.sub.len() && db2.sub[q].adsh == a && db2.sub[q].fy == Some(fy) && db2.sub[q].name == name;
            let s = db2.sub[q];
            if db.sub.contains(s) {
                let q2 = choose|q2: int| 0 <= q2 < db.sub.len() && db.sub[q2] == s;
                assert(sub_has(db, a, fy, name));
                assert(fm_has(fm, kall[i], fy, name));
                assert(u_pred_s(fm, kall, ds, fy, name));
            } else {
                let w = lemma_new_sub_in_delta(db, db2, vs, s);
                assert(vs[w].0 == sub_rt_view(ds[w].row));
                assert(sd_at(ds, w, fy, name));
                assert(u_pred_s(fm, kall, ds, fy, name));
            }
        }
        assert(u_has(u, fy, name));
    }
}

} // verus!

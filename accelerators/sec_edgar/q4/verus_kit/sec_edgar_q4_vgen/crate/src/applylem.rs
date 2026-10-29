// Facts about the lists apply builds from a transaction's rescans.
use vstd::prelude::*;
use verus_common::bag::*;
use verus_common::sums::*;
use crate::algebra::*;
use crate::emit::*;
use crate::gen_obligations::*;
use crate::cells::*;
use crate::facts::*;
use crate::gen_spec::*;
use crate::join_spec::*;
use crate::jspec::*;
use crate::lists::*;
use crate::rec::*;
use crate::rows::*;

verus! {

/// The rows of a filing rescan and of a tag rescan not of those filings, are the rows of either.
pub proof fn lemma_cover_sum<A>(out: Seq<A>, r1: Seq<A>, r2: Seq<A>, tab: Seq<A>, ka: spec_fn(A) -> bool, kg: spec_fn(A) -> bool)
    requires
        forall|f: spec_fn(A) -> int| #[trigger] sum(r1, f) == sum(tab, |x: A| if ka(x) { f(x) } else { 0 }),
        forall|f: spec_fn(A) -> int| #[trigger] sum(r2, f) == sum(tab, |x: A| if kg(x) { f(x) } else { 0 }),
        forall|f: spec_fn(A) -> int| #[trigger] sum(out, f) == sum(r1, f) + sum(r2, |x: A| if !ka(x) { f(x) } else { 0 }),
    ensures
        forall|f: spec_fn(A) -> int| #[trigger] sum(out, f) == sum(tab, |x: A| if ka(x) || kg(x) { f(x) } else { 0 }),
{
    assert forall|f: spec_fn(A) -> int| #[trigger] sum(out, f) == sum(tab, |x: A| if ka(x) || kg(x) { f(x) } else { 0 }) by {
        let f2 = |x: A| if !ka(x) { f(x) } else { 0 };
        let s1 = |x: A| if ka(x) { f(x) } else { 0 };
        let s2 = |x: A| if kg(x) { f2(x) } else { 0 };
        let both = |x: A| if ka(x) || kg(x) { f(x) } else { 0 };
        assert(sum(r2, f2) == sum(tab, s2));
        assert forall|x: A| #[trigger] both(x) == s1(x) + s2(x) by {}
        lemma_sum_add(tab, s1, s2, both);
    }
}

pub open spec fn num_ka(a: Seq<Seq<u8>>) -> spec_fn(NumRow) -> bool {
    |n: NumRow| kin(a, n.adsh)
}

pub open spec fn num_kg(g: Seq<Seq<u8>>) -> spec_fn(NumRow) -> bool {
    |n: NumRow| kin(g, n.tag)
}

/// The merged num rescans, with sign 1, stand for the num rows of the changed filings and tags.
pub proof fn lemma_nums_cover(db2: Db, keys_a: Seq<Vec<u8>>, rows_a: Seq<NumRt>, keys_g: Seq<Vec<u8>>, rows_g: Seq<NumRt>, out: Seq<NumRt>)
    requires
        rescan_nums_by_adsh_ok(db2, keys_a, rows_a),
        rescan_nums_by_tag_ok(db2, keys_g, rows_g),
        forall|f: spec_fn(NumRow) -> int| #[trigger] sum(rows_v(out), f)
            == sum(rows_v(rows_a), f) + sum(rows_v(rows_g), |x: NumRow| if !kin(seq_view_bytes(keys_a), <NumRt as AdshRow>::adsh_of(x)) { f(x) } else { 0 }),
    ensures
        forall|f: spec_fn(NumRow) -> int| #[trigger] wsum(ones(rows_v(out)), f)
            == sum(db2.num, |n: NumRow| if aff_n(seq_view_bytes(keys_a), seq_view_bytes(keys_g), n) { f(n) } else { 0 }),
{
    let (a, g) = (seq_view_bytes(keys_a), seq_view_bytes(keys_g));
    assert(rows_v(rows_a) =~= num_rt_seq_view(rows_a));
    assert(rows_v(rows_g) =~= num_rt_seq_view(rows_g));
    assert forall|f: spec_fn(NumRow) -> int| #[trigger] sum(rows_v(rows_a), f) == sum(db2.num, |n: NumRow| if num_ka(a)(n) { f(n) } else { 0 }) by {
        lemma_sum_ext_all(db2.num, |n: NumRow| if n.adsh is Some && a.contains(n.adsh->0) { f(n) } else { 0 },
            |n: NumRow| if num_ka(a)(n) { f(n) } else { 0 });
    }
    assert forall|f: spec_fn(NumRow) -> int| #[trigger] sum(rows_v(rows_g), f) == sum(db2.num, |n: NumRow| if num_kg(g)(n) { f(n) } else { 0 }) by {
        lemma_sum_ext_all(db2.num, |n: NumRow| if n.tag is Some && g.contains(n.tag->0) { f(n) } else { 0 },
            |n: NumRow| if num_kg(g)(n) { f(n) } else { 0 });
    }
    assert forall|f: spec_fn(NumRow) -> int| #[trigger] sum(rows_v(out), f)
        == sum(rows_v(rows_a), f) + sum(rows_v(rows_g), |x: NumRow| if !num_ka(a)(x) { f(x) } else { 0 }) by {
        lemma_sum_ext_all(rows_v(rows_g), |x: NumRow| if !kin(a, <NumRt as AdshRow>::adsh_of(x)) { f(x) } else { 0 }, |x: NumRow| if !num_ka(a)(x) { f(x) } else { 0 });
    }
    lemma_cover_sum(rows_v(out), rows_v(rows_a), rows_v(rows_g), db2.num, num_ka(a), num_kg(g));
    assert forall|f: spec_fn(NumRow) -> int| #[trigger] wsum(ones(rows_v(out)), f)
        == sum(db2.num, |n: NumRow| if aff_n(a, g, n) { f(n) } else { 0 }) by {
        lemma_ones_wsum(rows_v(out), f);
        lemma_sum_ext_all(db2.num, |n: NumRow| if num_ka(a)(n) || num_kg(g)(n) { f(n) } else { 0 },
            |n: NumRow| if aff_n(a, g, n) { f(n) } else { 0 });
    }
}

pub open spec fn pre_ka(a: Seq<Seq<u8>>) -> spec_fn(PreRow) -> bool {
    |r: PreRow| kin(a, r.adsh)
}

pub open spec fn pre_kg(g: Seq<Seq<u8>>) -> spec_fn(PreRow) -> bool {
    |r: PreRow| kin(g, r.tag)
}

/// The merged pre rescans stand for the pre rows of the changed filings and tags.
pub proof fn lemma_pres_cover(db2: Db, keys_a: Seq<Vec<u8>>, rows_a: Seq<PreRt>, keys_g: Seq<Vec<u8>>, rows_g: Seq<PreRt>, out: Seq<PreRt>)
    requires
        rescan_pres_by_adsh_ok(db2, keys_a, rows_a),
        rescan_pres_by_tag_ok(db2, keys_g, rows_g),
        forall|f: spec_fn(PreRow) -> int| #[trigger] sum(rows_v(out), f)
            == sum(rows_v(rows_a), f) + sum(rows_v(rows_g), |x: PreRow| if !kin(seq_view_bytes(keys_a), <PreRt as AdshRow>::adsh_of(x)) { f(x) } else { 0 }),
    ensures
        rtab_ok(seq_view_bytes(keys_a), seq_view_bytes(keys_g), db2.pre, rows_v(out)),
{
    let (a, g) = (seq_view_bytes(keys_a), seq_view_bytes(keys_g));
    assert(rows_v(rows_a) =~= pre_rt_seq_view(rows_a));
    assert(rows_v(rows_g) =~= pre_rt_seq_view(rows_g));
    assert forall|f: spec_fn(PreRow) -> int| #[trigger] sum(rows_v(rows_a), f) == sum(db2.pre, |r: PreRow| if pre_ka(a)(r) { f(r) } else { 0 }) by {
        lemma_sum_ext_all(db2.pre, |r: PreRow| if r.adsh is Some && a.contains(r.adsh->0) { f(r) } else { 0 },
            |r: PreRow| if pre_ka(a)(r) { f(r) } else { 0 });
    }
    assert forall|f: spec_fn(PreRow) -> int| #[trigger] sum(rows_v(rows_g), f) == sum(db2.pre, |r: PreRow| if pre_kg(g)(r) { f(r) } else { 0 }) by {
        lemma_sum_ext_all(db2.pre, |r: PreRow| if r.tag is Some && g.contains(r.tag->0) { f(r) } else { 0 },
            |r: PreRow| if pre_kg(g)(r) { f(r) } else { 0 });
    }
    assert forall|f: spec_fn(PreRow) -> int| #[trigger] sum(rows_v(out), f)
        == sum(rows_v(rows_a), f) + sum(rows_v(rows_g), |x: PreRow| if !pre_ka(a)(x) { f(x) } else { 0 }) by {
        lemma_sum_ext_all(rows_v(rows_g), |x: PreRow| if !kin(a, <PreRt as AdshRow>::adsh_of(x)) { f(x) } else { 0 }, |x: PreRow| if !pre_ka(a)(x) { f(x) } else { 0 });
    }
    lemma_cover_sum(rows_v(out), rows_v(rows_a), rows_v(rows_g), db2.pre, pre_ka(a), pre_kg(g));
    assert forall|f: spec_fn(PreRow) -> int| #[trigger] sum(rows_v(out), f)
        == sum(db2.pre, |r: PreRow| if aff_r(a, g, r) { f(r) } else { 0 }) by {
        lemma_sum_ext_all(db2.pre, |r: PreRow| if pre_ka(a)(r) || pre_kg(g)(r) { f(r) } else { 0 },
            |r: PreRow| if aff_r(a, g, r) { f(r) } else { 0 });
    }
}


/// Every merged num row is a row of a changed filing or tag.
pub proof fn lemma_nums_aff(db2: Db, keys_a: Seq<Vec<u8>>, rows_a: Seq<NumRt>, keys_g: Seq<Vec<u8>>, rows_g: Seq<NumRt>, out: Seq<NumRt>)
    requires
        rescan_nums_by_adsh_ok(db2, keys_a, rows_a),
        rescan_nums_by_tag_ok(db2, keys_g, rows_g),
        forall|i: int| 0 <= i < out.len() ==> rows_v(rows_a).contains((#[trigger] out[i]).rv())
            || (rows_v(rows_g).contains(out[i].rv()) && !kin(seq_view_bytes(keys_a), out[i].rv().adsh)),
    ensures
        forall|i: int| 0 <= i < out.len() ==> aff_n(seq_view_bytes(keys_a), seq_view_bytes(keys_g), (#[trigger] out[i]).rv()),
{
    let (a, g) = (seq_view_bytes(keys_a), seq_view_bytes(keys_g));
    assert(rows_v(rows_a) =~= num_rt_seq_view(rows_a));
    assert(rows_v(rows_g) =~= num_rt_seq_view(rows_g));
    assert forall|f: spec_fn(NumRow) -> int| #[trigger] sum(rows_v(rows_a), f) == sum(db2.num, |n: NumRow| if num_ka(a)(n) { f(n) } else { 0 }) by {
        lemma_sum_ext_all(db2.num, |n: NumRow| if n.adsh is Some && a.contains(n.adsh->0) { f(n) } else { 0 },
            |n: NumRow| if num_ka(a)(n) { f(n) } else { 0 });
    }
    assert forall|f: spec_fn(NumRow) -> int| #[trigger] sum(rows_v(rows_g), f) == sum(db2.num, |n: NumRow| if num_kg(g)(n) { f(n) } else { 0 }) by {
        lemma_sum_ext_all(db2.num, |n: NumRow| if n.tag is Some && g.contains(n.tag->0) { f(n) } else { 0 },
            |n: NumRow| if num_kg(g)(n) { f(n) } else { 0 });
    }
    lemma_rescan_rows(rows_v(rows_a), db2.num, num_ka(a));
    lemma_rescan_rows(rows_v(rows_g), db2.num, num_kg(g));
    assert forall|i: int| 0 <= i < out.len() implies aff_n(a, g, (#[trigger] out[i]).rv()) by {
        if rows_v(rows_a).contains(out[i].rv()) {
            let j = choose|j: int| 0 <= j < rows_v(rows_a).len() && rows_v(rows_a)[j] == out[i].rv();
            assert(num_ka(a)(rows_v(rows_a)[j]));
        } else {
            let j = choose|j: int| 0 <= j < rows_v(rows_g).len() && rows_v(rows_g)[j] == out[i].rv();
            assert(num_kg(g)(rows_v(rows_g)[j]));
        }
    }
}


/// The new total is the old total plus the records of the three passes.
pub proof fn lemma_apply_total(
    db: Db, db2: Db,
    dn: Seq<(NumRow, int)>, dp: Seq<(PreRow, int)>, ds: Seq<(SubRow, int)>, dt: Seq<(TagRow, int)>,
    a: Seq<Seq<u8>>, g: Seq<Seq<u8>>, nl: Seq<(NumRow, int)>, rtab: Seq<PreRow>, recs: Seq<Rec>,
)
    requires
        bag_step(db.num, db2.num, dn),
        bag_step(db.pre, db2.pre, dp),
        unit_signs(dp),
        bag_step(db.sub, db2.sub, ds),
        bag_step(db.tag, db2.tag, dt),
        forall|i: int| 0 <= i < dn.len() && (#[trigger] dn[i]).0.adsh is Some ==> a.contains(dn[i].0.adsh->0),
        forall|i: int| 0 <= i < dp.len() && (#[trigger] dp[i]).0.adsh is Some ==> a.contains(dp[i].0.adsh->0),
        forall|i: int| 0 <= i < ds.len() ==> a.contains((#[trigger] ds[i]).0.adsh),
        forall|i: int| 0 <= i < dt.len() ==> g.contains((#[trigger] dt[i]).0.tag),
        forall|f: spec_fn(NumRow) -> int| #[trigger] wsum(nl, f)
            == sum(db2.num, |n: NumRow| if aff_n(a, g, n) { f(n) } else { 0 }),
        forall|i: int| 0 <= i < nl.len() ==> (#[trigger] nl[i]).1 == 0 || aff_n(a, g, nl[i].0),
        rtab_ok(a, g, db2.pre, rtab),
        forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(recs, p, h)
            == passv(nl, db2.sub, db2.tag, new_sides(rtab), p, h)
                - passv(nl, db.sub, db.tag, old_sides(rtab, dp), p, h)
                + passv(dn, db.sub, db.tag, old_sides(rtab, dp), p, h),
    ensures
        forall|p: Params, h: spec_fn(JK) -> int| #[trigger] tot_recs(tot_db(db), recs)(p, h) == tot_db(db2)(p, h),
{
    assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] tot_recs(tot_db(db), recs)(p, h) == tot_db(db2)(p, h) by {
        lemma_delta(db, db2, dn, dp, ds, dt, a, g, nl, rtab, p, h);
        lemma_tuple_jsum(db, p, h);
        lemma_tuple_jsum(db2, p, h);
    }
}

}

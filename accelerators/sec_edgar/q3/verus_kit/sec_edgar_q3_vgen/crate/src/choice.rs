// The spec's rounded numbers, chosen from a class's entries: a total per company, the average of those,
// a total per group; and the proof that they are within their bounds.
use vstd::prelude::*;
use verus_common::order::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::lemma_sum_same_support;
use verus_prooflib::lemma_sum_ones;
use verus_prooflib::float::*;
use crate::ent::*;
use crate::fin::*;
use crate::gen_spec::*;
use crate::gkey::*;
use crate::qty::*;

verus! {

/// The rounded total chosen for a company.
pub open spec fn entry_cv(es: Seq<Ent>, k: Option<int>) -> int {
    if exists|i: int| 0 <= i < es.len() && opt_int((#[trigger] es[i]).cik) == k {
        es[choose|i: int| 0 <= i < es.len() && opt_int((#[trigger] es[i]).cik) == k].cv@
    } else {
        0
    }
}

pub proof fn lemma_entry_cv(es: Seq<Ent>, i: int)
    requires
        0 <= i < es.len(),
        forall|a: int, b: int| 0 <= a < b < es.len() ==> (#[trigger] es[a]).cik != (#[trigger] es[b]).cik,
    ensures
        entry_cv(es, opt_int(es[i].cik)) == es[i].cv@,
{
    let k = opt_int(es[i].cik);
    let j = choose|j: int| 0 <= j < es.len() && opt_int((#[trigger] es[j]).cik) == k;
    assert(opt_int(es[j].cik) == opt_int(es[i].cik));
    assert(es[j].cik == es[i].cik);
    if j != i {
        if j < i { assert(es[j].cik != es[i].cik); } else { assert(es[i].cik != es[j].cik); }
    }
}

/// The rounded total of a group.
pub open spec fn ocv(os: Seq<Ent>, gn: Option<Seq<u8>>, gc: Option<int>) -> int {
    if exists|i: int| 0 <= i < os.len() && opt_bytes((#[trigger] os[i]).name) == gn && opt_int(os[i].cik) == gc {
        os[choose|i: int| 0 <= i < os.len() && opt_bytes((#[trigger] os[i]).name) == gn && opt_int(os[i].cik) == gc].cv@
    } else {
        0
    }
}

pub proof fn lemma_ocv(os: Seq<Ent>, i: int)
    requires
        0 <= i < os.len(),
        forall|a: int, b: int| 0 <= a < b < os.len() ==>
            !(opt_bytes((#[trigger] os[a]).name) == opt_bytes((#[trigger] os[b]).name) && os[a].cik == os[b].cik),
    ensures
        ocv(os, opt_bytes(os[i].name), opt_int(os[i].cik)) == os[i].cv@,
{
    let (gn, gc) = (opt_bytes(os[i].name), opt_int(os[i].cik));
    let j = choose|j: int| 0 <= j < os.len() && opt_bytes((#[trigger] os[j]).name) == gn && opt_int(os[j].cik) == gc;
    assert(os[j].cik == os[i].cik);
    if j != i {
        if j < i { assert(!(opt_bytes(os[j].name) == opt_bytes(os[i].name) && os[j].cik == os[i].cik)); }
        else { assert(!(opt_bytes(os[i].name) == opt_bytes(os[j].name) && os[i].cik == os[j].cik)); }
    }
}

pub proof fn lemma_abs_sum_cv(es: Seq<Ent>)
    ensures abs(sum(es, |e: Ent| e.cv@)) <= sum(es, |e: Ent| abs(e.cv@)),
    decreases es.len(),
{
    if es.len() > 0 {
        let rest = es.drop_last();
        assert(es =~= rest.push(es.last()));
        lemma_abs_sum_cv(rest);
        lemma_sum_push(rest, es.last(), |e: Ent| e.cv@);
        lemma_sum_push(rest, es.last(), |e: Ent| abs(e.cv@));
    } else {
        assert(es =~= Seq::<Ent>::empty());
    }
}

proof fn lemma_map_where_true(s: Seq<AvgSubTuple>, f: spec_fn(AvgSubTuple) -> Option<int>, keep: spec_fn(AvgSubTuple) -> bool)
    requires forall|t: AvgSubTuple| #[trigger] keep(t),
    ensures map_where(s, keep, f) == s.map_values(f),
    decreases s.len(),
{
    if s.len() > 0 {
        lemma_map_where_true(s.drop_last(), f, keep);
        assert(s.map_values(f) =~= s.drop_last().map_values(f).push(f(s.last())));
    }
}

/// The companies the subquery groups by: those with a tuple.
proof fn lemma_ks_contains(db: Db, p: Params, ch: spec_fn(Option<int>) -> int, k: Option<int>)
    ensures
        dedup(map_where(avg_sub_tuples(db, p), |t: AvgSubTuple| avg_sub_having(db, p, ch, t.s2.cik), |t: AvgSubTuple| t.s2.cik)).contains(k)
            == (avg_sub_group_sum(db, p, k, avg_vs()) > 0),
{
    let tup = avg_sub_tuples(db, p);
    let keep = |t: AvgSubTuple| avg_sub_having(db, p, ch, t.s2.cik);
    let f = |t: AvgSubTuple| t.s2.cik;
    let ciks = map_where(tup, keep, f);
    lemma_dedup(ciks);
    lemma_map_where(tup, keep, f);
    lemma_count_vs(db, p, None, k);
    let ind = |t: AvgSubTuple| if t.s2.cik == k { 1int } else { 0int };
    assert(avg_sub_group_count(db, p, k) == sum(tup, ind));
    assert forall|i: int| 0 <= i < tup.len() implies ind(#[trigger] tup[i]) >= 0 by {}
    if ciks.contains(k) {
        let i = choose|i: int| 0 <= i < tup.len() && keep(tup[i]) && f(tup[i]) == k;
        lemma_sum_pos(tup, ind, i);
    }
    if sum(tup, ind) > 0 {
        let i = lemma_sum_nonzero_witness(tup, ind);
        assert(keep(tup[i]) && f(tup[i]) == k);
    }
}

/// The scalar block's sums over the rows of the subquery are the sums over the companies.
pub proof fn lemma_scalar(db: Db, p: Params, es: Seq<Ent>, ch: spec_fn(Option<int>) -> int)
    requires
        ients_ok(db, p, es),
        forall|i: int| 0 <= i < es.len() ==> ch(opt_int((#[trigger] es[i]).cik)) == es[i].cv@,
    ensures
        scalar_1_group_sum(db, p, ch, |t: Scalar1Tuple| if t.avg_sub.sub_total is Some { 1int } else { 0 }) == es.len() as int,
        scalar_1_group_sum(db, p, ch, |t: Scalar1Tuple| if t.avg_sub.sub_total is Some { t.avg_sub.sub_total->0 } else { 0 })
            == sum(es, |e: Ent| e.cv@),
        scalar_1_group_sum(db, p, ch, |t: Scalar1Tuple| if t.avg_sub.sub_total is Some { abs(t.avg_sub.sub_total->0) } else { 0 })
            == sum(es, |e: Ent| abs(e.cv@)),
{
    let tup = avg_sub_tuples(db, p);
    let keep = |t: AvgSubTuple| avg_sub_having(db, p, ch, t.s2.cik);
    let f = |t: AvgSubTuple| t.s2.cik;
    lemma_map_where_true(tup, f, keep);
    let ks = dedup(tup.map_values(f));
    assert(ks == dedup(map_where(tup, keep, f)));
    lemma_dedup(map_where(tup, keep, f));
    let rows = rel_avg_sub(db, p, ch);
    let mk = |k: Option<int>| avg_sub_row_of(db, p, ch, k);
    assert(rows == ks.map_values(mk));
    let ekeys = es.map_values(|e: Ent| opt_int(e.cik));
    // the same companies, each once
    assert forall|k: Option<int>| ks.contains(k) == ekeys.contains(k) by {
        lemma_ks_contains(db, p, ch, k);
        if ekeys.contains(k) {
            let i = choose|i: int| 0 <= i < ekeys.len() && ekeys[i] == k;
            assert(ekeys[i] == opt_int(es[i].cik));
            assert(avg_sub_group_sum(db, p, k, avg_vs()) == es[i].count@);
        }
        if ks.contains(k) {
            assert(avg_sub_group_sum(db, p, k, avg_vs()) > 0);
            let i = choose|i: int| 0 <= i < es.len() && opt_int((#[trigger] es[i]).cik) == k;
            assert(ekeys[i] == k);
        }
    }
    assert(ekeys.no_duplicates()) by {
        assert forall|i: int, j: int| 0 <= i < j < ekeys.len() implies ekeys[i] != ekeys[j] by {
            assert(es[i].cik != es[j].cik);
        }
    }
    assert(ks.no_duplicates());
    assert(ks.to_set() =~= ekeys.to_set()) by {
        ks.to_set_ensures();
        ekeys.to_set_ensures();
    }
    // a row of a company with tuples is its chosen total
    assert forall|k: Option<int>| ks.contains(k) implies (#[trigger] mk(k)).sub_total == Some(ch(k)) by {
        lemma_ks_contains(db, p, ch, k);
    }
    let one_s = |t: Scalar1Tuple| if t.avg_sub.sub_total is Some { 1int } else { 0 };
    let val_s = |t: Scalar1Tuple| if t.avg_sub.sub_total is Some { t.avg_sub.sub_total->0 } else { 0 };
    let abs_s = |t: Scalar1Tuple| if t.avg_sub.sub_total is Some { abs(t.avg_sub.sub_total->0) } else { 0 };
    let w = |r: AvgSubRow| Scalar1Tuple { avg_sub: r };
    let g1 = |r: AvgSubRow| scalar_1_tuples_1(db, p, ch, r);
    lemma_sum_flat_map(rows, g1, one_s);
    lemma_sum_flat_map(rows, g1, val_s);
    lemma_sum_flat_map(rows, g1, abs_s);
    assert forall|r: AvgSubRow| #[trigger] g1(r) == seq![w(r)] by {}
    let by_row_one = |r: AvgSubRow| sum(g1(r), one_s);
    let by_row_val = |r: AvgSubRow| sum(g1(r), val_s);
    let by_row_abs = |r: AvgSubRow| sum(g1(r), abs_s);
    assert forall|r: AvgSubRow| #[trigger] by_row_one(r) == one_s(w(r)) by { lemma_sum_singleton(w(r), one_s); }
    assert forall|r: AvgSubRow| #[trigger] by_row_val(r) == val_s(w(r)) by { lemma_sum_singleton(w(r), val_s); }
    assert forall|r: AvgSubRow| #[trigger] by_row_abs(r) == abs_s(w(r)) by { lemma_sum_singleton(w(r), abs_s); }
    let h1 = |k: Option<int>| one_s(w(mk(k)));
    let hv = |k: Option<int>| val_s(w(mk(k)));
    let ha = |k: Option<int>| abs_s(w(mk(k)));
    lemma_sum_map(ks, mk, |r: AvgSubRow| one_s(w(r)), h1);
    lemma_sum_map(ks, mk, |r: AvgSubRow| val_s(w(r)), hv);
    lemma_sum_map(ks, mk, |r: AvgSubRow| abs_s(w(r)), ha);
    lemma_sum_ext_all(rows, by_row_one, |r: AvgSubRow| one_s(w(r)));
    lemma_sum_ext_all(rows, by_row_val, |r: AvgSubRow| val_s(w(r)));
    lemma_sum_ext_all(rows, by_row_abs, |r: AvgSubRow| abs_s(w(r)));
    lemma_sum_same_support(ks, ekeys, h1);
    lemma_sum_same_support(ks, ekeys, hv);
    lemma_sum_same_support(ks, ekeys, ha);
    let e1 = |e: Ent| 1int;
    let ev = |e: Ent| e.cv@;
    let ea = |e: Ent| abs(e.cv@);
    let gk_ = |e: Ent| opt_int(e.cik);
    let hh1 = |e: Ent| h1(opt_int(e.cik));
    let hhv = |e: Ent| hv(opt_int(e.cik));
    let hha = |e: Ent| ha(opt_int(e.cik));
    lemma_sum_map(es, gk_, h1, hh1);
    lemma_sum_map(es, gk_, hv, hhv);
    lemma_sum_map(es, gk_, ha, hha);
    assert forall|i: int| 0 <= i < es.len() implies hh1(#[trigger] es[i]) == e1(es[i]) && hhv(es[i]) == ev(es[i]) && hha(es[i]) == ea(es[i]) by {
        assert(ekeys[i] == opt_int(es[i].cik));
        assert(ekeys.contains(opt_int(es[i].cik)));
        assert(ks.contains(opt_int(es[i].cik)));
        assert(mk(opt_int(es[i].cik)).sub_total == Some(ch(opt_int(es[i].cik))));
    }
    lemma_sum_ext(es, hh1, e1);
    lemma_sum_ext(es, hhv, ev);
    lemma_sum_ext(es, hha, ea);
    lemma_sum_ones(es);
}

/// A rounded quotient is within the bound of float_within_div.
pub proof fn lemma_avg_within(v: int, q: int, e: int, a: int, k: int)
    requires
        k >= 1,
        2 * abs(q * k - e) <= k,
        abs(q) <= abs(e),
        abs(e) <= a,
        v == q || pow2(53) * abs(v - q) <= abs(q),
    ensures
        float_within_div(v * k, e, a, k, k),
{
    let big = pow2(53);
    if v * k == e || k >= big {
    } else {
        let d = abs(v - q);
        let t = abs(q * k - e);
        assert(d >= 0 && t >= 0);
        assert(big * d <= abs(q)) by {
            if v == q {
                assert(d == 0);
                assert(big * d == 0) by (nonlinear_arith) requires d == 0;
            }
        }
        assert(abs(v * k - e) <= k * d + t) by (nonlinear_arith)
            requires d == abs(v - q), t == abs(q * k - e), k >= 1;
        assert(2 * (big - k) * abs(v * k - e) <= 2 * k * a + (big - k) * abs(k)) by (nonlinear_arith)
            requires
                d >= 0, t >= 0, k >= 1, k < big,
                big * d <= abs(q), abs(q) <= a, 2 * t <= k,
                abs(v * k - e) <= k * d + t, abs(k) == k;
    }
}

/// The three numbers the engine chooses, with their bounds.
pub proof fn lemma_chosen(db: Db, p: Params, es: Seq<Ent>, os: Seq<Ent>, q: int, cvs: int, bits_s: Option<u64>)
    requires
        ients_ok(db, p, es),
        gents_ok(db, p, os),
        es.len() >= 1 ==> 2 * abs(q * es.len() as int - sum(es, |e: Ent| e.cv@)) <= es.len() as int,
        es.len() >= 1 ==> abs(q) <= abs(sum(es, |e: Ent| e.cv@)),
        es.len() >= 1 ==> rounded_as(q, cvs, bits_s),
    ensures ({
        let ch_a = |k: Option<int>| entry_cv(es, k);
        let ch_1 = |gn: Option<Seq<u8>>, gc: Option<int>| ocv(os, gn, gc);
        chosen(db, p, ch_a, cvs, ch_1)
    }),
{
    let ch_a = |k: Option<int>| entry_cv(es, k);
    let ch_1 = |gn: Option<Seq<u8>>, gc: Option<int>| ocv(os, gn, gc);
    assert forall|i: int| 0 <= i < es.len() implies ch_a(opt_int((#[trigger] es[i]).cik)) == es[i].cv@ by {
        lemma_entry_cv(es, i);
    }
    // per company
    assert forall|gc: Option<int>| #![trigger ch_a(gc)] avg_sub_group_sum(db, p, gc, avg_vs()) > 0 implies
        float_within(ch_a(gc), avg_sub_group_sum(db, p, gc, avg_value()), avg_sub_group_sum(db, p, gc, avg_mag()),
            avg_sub_group_sum(db, p, gc, avg_vs()) - 1) by {
        let i = choose|i: int| 0 <= i < es.len() && opt_int((#[trigger] es[i]).cik) == gc;
        assert(ient_ok(db, p, es[i]));
        assert(ch_a(gc) == es[i].cv@);
    }
    // the scalar average
    lemma_scalar(db, p, es, ch_a);
    lemma_abs_sum_cv(es);
    let big_k = es.len() as int;
    let (s, a) = (sum(es, |e: Ent| e.cv@), sum(es, |e: Ent| abs(e.cv@)));
    if big_k >= 1 {
        match bits_s {
            Some(b) => { lemma_avg_within(cvs, q, s, a, big_k); },
            None => { lemma_avg_within(q, q, s, a, big_k); },
        }
    }
    // per group
    assert forall|gn: Option<Seq<u8>>, gc: Option<int>| #![trigger ch_1(gn, gc)] group_sum(db, p, gn, gc, tup_vs()) > 0 implies
        float_within(ch_1(gn, gc), group_sum(db, p, gn, gc, tup_value()), group_sum(db, p, gn, gc, tup_mag()),
            group_sum(db, p, gn, gc, tup_vs()) - 1) by {
        let i = choose|i: int| 0 <= i < os.len() && opt_bytes((#[trigger] os[i]).name) == gn && opt_int(os[i].cik) == gc;
        assert(gent_ok(db, p, os[i]));
        lemma_ocv(os, i);
    }
}

}

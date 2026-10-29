// The subquery: one rounded sum per company, and the sums of those rounded numbers that the scalar
// average reads.
use vstd::prelude::*;
use verus_common::order::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::lemma_sum_same_support;
use verus_prooflib::lemma_sum_ones;
use verus_prooflib::wide::Wide;
use crate::bridge::*;
use crate::gen_spec::*;
use crate::groups::*;
use crate::join::*;
use crate::join_spec::*;

verus! {

/// A company's total, rounded.
pub struct IEntry {
    pub cik: Option<i64>,
    pub cv: Wide,
    pub count: usize,
    pub sum: Wide,
    pub abs: Wide,
}

pub open spec fn entry_ok(l: Seq<Rec>, e: IEntry) -> bool {
    &&& e.count as int == isum(l, opt_int(e.cik), one())
    &&& e.count >= 1
    &&& e.sum@ == isum(l, opt_int(e.cik), value())
    &&& e.abs@ == isum(l, opt_int(e.cik), magnitude())
    &&& float_within(e.cv@, e.sum@, e.abs@, e.count as int - 1)
}

pub open spec fn entries_ok(l: Seq<Rec>, es: Seq<IEntry>) -> bool {
    &&& forall|i: int| 0 <= i < es.len() ==> entry_ok(l, #[trigger] es[i])
    &&& forall|i: int, j: int| 0 <= i < j < es.len() ==> (#[trigger] es[i]).cik != (#[trigger] es[j]).cik
    &&& forall|k: Option<int>| isum(l, k, one()) > 0 ==> exists|i: int| 0 <= i < es.len() && opt_int((#[trigger] es[i]).cik) == k
}

/// The rounded total chosen for a company.
pub open spec fn entry_cv(es: Seq<IEntry>, k: Option<int>) -> int {
    if exists|i: int| 0 <= i < es.len() && opt_int((#[trigger] es[i]).cik) == k {
        es[choose|i: int| 0 <= i < es.len() && opt_int((#[trigger] es[i]).cik) == k].cv@
    } else {
        0
    }
}

pub proof fn lemma_entry_cv(es: Seq<IEntry>, i: int)
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

/// The companies the subquery groups by: those with a record.
proof fn lemma_ks_contains(db: Db, p: Params, l: Seq<Rec>, ch: spec_fn(Option<int>) -> int, k: Option<int>)
    requires records_ok(db, p, l),
    ensures
        dedup(map_where(avg_sub_tuples(db, p), |t: AvgSubTuple| avg_sub_having(db, p, ch, t.s2.cik), |t: AvgSubTuple| t.s2.cik)).contains(k)
            == (isum(l, k, one()) > 0),
{
    let tup = avg_sub_tuples(db, p);
    let keep = |t: AvgSubTuple| avg_sub_having(db, p, ch, t.s2.cik);
    let f = |t: AvgSubTuple| t.s2.cik;
    let ciks = map_where(tup, keep, f);
    lemma_dedup(ciks);
    lemma_map_where(tup, keep, f);
    lemma_inner_quantities(db, p, l, k);
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

/// The scalar block's sums over the rows of the subquery are the sums over the entries.
pub proof fn lemma_scalar(db: Db, p: Params, l: Seq<Rec>, es: Seq<IEntry>, ch: spec_fn(Option<int>) -> int)
    requires
        records_ok(db, p, l),
        entries_ok(l, es),
        forall|i: int| 0 <= i < es.len() ==> ch(opt_int((#[trigger] es[i]).cik)) == es[i].cv@,
    ensures
        scalar_1_group_sum(db, p, ch, |t: Scalar1Tuple| if t.avg_sub.sub_total is Some { 1int } else { 0 }) == es.len() as int,
        scalar_1_group_sum(db, p, ch, |t: Scalar1Tuple| if t.avg_sub.sub_total is Some { t.avg_sub.sub_total->0 } else { 0 })
            == sum(es, |e: IEntry| e.cv@),
        scalar_1_group_sum(db, p, ch, |t: Scalar1Tuple| if t.avg_sub.sub_total is Some { abs(t.avg_sub.sub_total->0) } else { 0 })
            == sum(es, |e: IEntry| abs(e.cv@)),
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
    let ekeys = es.map_values(|e: IEntry| opt_int(e.cik));
    // the same companies, each once
    assert forall|k: Option<int>| ks.contains(k) == ekeys.contains(k) by {
        lemma_ks_contains(db, p, l, ch, k);
        if ekeys.contains(k) {
            let i = choose|i: int| 0 <= i < ekeys.len() && ekeys[i] == k;
            assert(ekeys[i] == opt_int(es[i].cik));
            assert(isum(l, k, one()) == es[i].count as int);
        }
        if ks.contains(k) {
            assert(isum(l, k, one()) > 0);
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
    // a row of a company with records is its chosen total
    let sf = |k: Option<int>| 1int;
    assert forall|k: Option<int>| ks.contains(k) implies (#[trigger] mk(k)).sub_total == Some(ch(k)) by {
        lemma_ks_contains(db, p, l, ch, k);
        lemma_inner_quantities(db, p, l, k);
    }
    let one_s = |t: Scalar1Tuple| if t.avg_sub.sub_total is Some { 1int } else { 0 };
    let val_s = |t: Scalar1Tuple| if t.avg_sub.sub_total is Some { t.avg_sub.sub_total->0 } else { 0 };
    let abs_s = |t: Scalar1Tuple| if t.avg_sub.sub_total is Some { abs(t.avg_sub.sub_total->0) } else { 0 };
    // sum over the scalar tuples = sum over the rows = sum over the companies
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
    let e1 = |e: IEntry| 1int;
    let ev = |e: IEntry| e.cv@;
    let ea = |e: IEntry| abs(e.cv@);
    let gk_ = |e: IEntry| opt_int(e.cik);
    let hh1 = |e: IEntry| h1(opt_int(e.cik));
    let hhv = |e: IEntry| hv(opt_int(e.cik));
    let hha = |e: IEntry| ha(opt_int(e.cik));
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

}

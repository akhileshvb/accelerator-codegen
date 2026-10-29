// The rounded numbers the answer is built from, as the spec's choices: a total per company, the
// scalar average of those, a total per group; and the proof that they are within their bounds.
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::float::*;
use verus_prooflib::wide::Wide;
use crate::bridge::*;
use crate::entries::*;
use crate::gen_spec::*;
use crate::groups::*;
use crate::join::*;
use crate::join_spec::*;
use crate::scalar::*;

verus! {

pub open spec fn erase_rec(r: Rec) -> Rec {
    Rec { name: None, cik: r.cik, v: r.v, vb: r.vb }
}

pub open spec fn erased(l: Seq<Rec>) -> Seq<Rec> {
    l.map_values(|r: Rec| erase_rec(r))
}

/// The company-level sums over the records are the group sums over the records with their names erased.
pub proof fn lemma_erased_sum(l: Seq<Rec>, k: Option<int>, f: spec_fn(JK) -> int)
    requires forall|j: JK| #[trigger] f(JK { name: None, ..j }) == f(j),
    ensures gsum(erased(l), None, k, f) == isum(l, k, f),
{
    let g = |r: Rec| if in_group(r, None, k) { f(rec_jk(r)) } else { 0 };
    let h = |r: Rec| if rec_jk(r).cik == k { f(rec_jk(r)) } else { 0 };
    assert forall|r: Rec| #[trigger] g(erase_rec(r)) == h(r) by {
        assert(rec_jk(erase_rec(r)) == (JK { name: None, ..rec_jk(r) }));
        assert(f(JK { name: None, ..rec_jk(r) }) == f(rec_jk(r)));
    }
    lemma_sum_map(l, |r: Rec| erase_rec(r), g, h);
}

proof fn lemma_erased_sums(l: Seq<Rec>, k: Option<int>)
    ensures
        gsum(erased(l), None, k, one()) == isum(l, k, one()),
        gsum(erased(l), None, k, value()) == isum(l, k, value()),
        gsum(erased(l), None, k, magnitude()) == isum(l, k, magnitude()),
{
    assert forall|j: JK| #[trigger] one()(JK { name: None, ..j }) == one()(j) by {}
    assert forall|j: JK| #[trigger] value()(JK { name: None, ..j }) == value()(j) by {}
    assert forall|j: JK| #[trigger] magnitude()(JK { name: None, ..j }) == magnitude()(j) by {}
    lemma_erased_sum(l, k, one());
    lemma_erased_sum(l, k, value());
    lemma_erased_sum(l, k, magnitude());
}

/// Only the nameless group has records once the names are erased.
proof fn lemma_erased_name(l: Seq<Rec>, gn: Option<Seq<u8>>, gc: Option<int>)
    requires gsum(erased(l), gn, gc, one()) > 0,
    ensures gn is None,
{
    let l2 = erased(l);
    let g = |r: Rec| if in_group(r, gn, gc) { one()(rec_jk(r)) } else { 0 };
    let i = lemma_sum_nonzero_witness(l2, g);
    assert(l2[i] == erase_rec(l[i]));
    assert(in_group(l2[i], gn, gc));
}

pub open spec fn ientry_of(g: GEntry) -> IEntry {
    IEntry { cik: g.cik, cv: g.cv, count: g.count, sum: g.sum, abs: g.abs }
}

pub open spec fn ientries(gs: Seq<GEntry>) -> Seq<IEntry> {
    gs.map_values(|g: GEntry| ientry_of(g))
}

/// The entries of the nameless groups of the erased records are the subquery's entries.
pub proof fn lemma_inner_entries(l: Seq<Rec>, gs: Seq<GEntry>)
    requires gentries_ok(erased(l), gs),
    ensures entries_ok(l, ientries(gs)),
{
    let es = ientries(gs);
    assert forall|i: int| 0 <= i < es.len() implies entry_ok(l, #[trigger] es[i]) by {
        let g = gs[i];
        assert(es[i] == ientry_of(g));
        assert(gsum(erased(l), opt_bytes(g.name), opt_int(g.cik), one()) > 0);
        lemma_erased_name(l, opt_bytes(g.name), opt_int(g.cik));
        assert(opt_bytes(g.name) is None);
        lemma_erased_sums(l, opt_int(g.cik));
    }
    assert forall|i: int, j: int| 0 <= i < j < es.len() implies (#[trigger] es[i]).cik != (#[trigger] es[j]).cik by {
        let (a, b) = (gs[i], gs[j]);
        lemma_erased_name(l, opt_bytes(a.name), opt_int(a.cik));
        lemma_erased_name(l, opt_bytes(b.name), opt_int(b.cik));
        assert(opt_bytes(a.name) == opt_bytes(b.name));
    }
    assert forall|k: Option<int>| isum(l, k, one()) > 0 implies exists|i: int| 0 <= i < es.len() && opt_int((#[trigger] es[i]).cik) == k by {
        lemma_erased_sums(l, k);
        let i = choose|i: int| 0 <= i < gs.len() && opt_bytes((#[trigger] gs[i]).name) == None::<Seq<u8>> && opt_int(gs[i].cik) == k;
        assert(es[i] == ientry_of(gs[i]));
    }
}

/// The rounded total of a group.
pub open spec fn ocv(os: Seq<GEntry>, gn: Option<Seq<u8>>, gc: Option<int>) -> int {
    if exists|i: int| 0 <= i < os.len() && opt_bytes((#[trigger] os[i]).name) == gn && opt_int(os[i].cik) == gc {
        os[choose|i: int| 0 <= i < os.len() && opt_bytes((#[trigger] os[i]).name) == gn && opt_int(os[i].cik) == gc].cv@
    } else {
        0
    }
}

pub proof fn lemma_ocv(os: Seq<GEntry>, i: int)
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

pub proof fn lemma_abs_sum_cv(es: Seq<IEntry>)
    ensures abs(sum(es, |e: IEntry| e.cv@)) <= sum(es, |e: IEntry| abs(e.cv@)),
    decreases es.len(),
{
    if es.len() > 0 {
        let rest = es.drop_last();
        assert(es =~= rest.push(es.last()));
        lemma_abs_sum_cv(rest);
        lemma_sum_push(rest, es.last(), |e: IEntry| e.cv@);
        lemma_sum_push(rest, es.last(), |e: IEntry| abs(e.cv@));
    } else {
        assert(es =~= Seq::<IEntry>::empty());
    }
}

/// The three numbers the engine chooses, with their bounds.
pub proof fn lemma_chosen(db: Db, p: Params, l: Seq<Rec>, es: Seq<IEntry>, os: Seq<GEntry>,
    q: int, cvs: int, bits_s: Option<u64>)
    requires
        records_ok(db, p, l),
        entries_ok(l, es),
        gentries_ok(l, os),
        es.len() >= 1 ==> 2 * abs(q * es.len() as int - sum(es, |e: IEntry| e.cv@)) <= es.len() as int,
        es.len() >= 1 ==> abs(q) <= abs(sum(es, |e: IEntry| e.cv@)),
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
        lemma_inner_quantities(db, p, l, gc);
        let i = choose|i: int| 0 <= i < es.len() && opt_int((#[trigger] es[i]).cik) == gc;
        assert(entry_ok(l, es[i]));
        assert(ch_a(gc) == es[i].cv@);
    }
    // the scalar average
    lemma_scalar(db, p, l, es, ch_a);
    lemma_abs_sum_cv(es);
    let big_k = es.len() as int;
    let (s, a) = (sum(es, |e: IEntry| e.cv@), sum(es, |e: IEntry| abs(e.cv@)));
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
        lemma_outer_quantities(db, p, l, gn, gc);
        let i = choose|i: int| 0 <= i < os.len() && opt_bytes((#[trigger] os[i]).name) == gn && opt_int(os[i].cik) == gc;
        assert(gentry_ok(l, os[i]));
        lemma_ocv(os, i);
    }
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

}

// Reading the answer: the groups of the asked triple, rounded to doubles, sorted, cut at LIMIT.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_common::bytes::*;
use verus_prooflib::ordered::{Index, Key, KeyView, compare_bytes};
use verus_prooflib::wide::*;
use verus_prooflib::float::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::algebra::*;
use crate::esc::*;
use crate::accmap::*;
use crate::gmap::*;
use crate::merge::*;
use crate::wideval::*;
use crate::f64bits::*;
use crate::glue_proofs::*;
use crate::rep::*;
use crate::answer::*;
use crate::ordering::*;
use crate::kernel::*;
use crate::merge::arc_val;

verus! {

pub struct OutR {
    pub name: Option<Vec<u8>>,
    pub tag: Vec<u8>,
    pub plabel: Option<Vec<u8>>,
    pub cnt: Wide,
    /// the chosen total, exactly (the double's value, or the exact sum when out of range)
    pub vw: Wide,
    pub bits: Option<u64>,
}

pub open spec fn rview(r: OutR, p2: Seq<u8>) -> OutRow {
    OutRow { name: ov(r.name), stmt: Some(p2), tag: Some(r.tag@), plabel: ov(r.plabel), total_value: Some(r.vw@), cnt: r.cnt@ }
}

pub open spec fn max_val() -> int {
    (pow2(53) - 1) * pow2(2045)
}

/// A row of the answer from a stored group.
pub fn make_row(tab: &Pow2Tab, maxv: &Wide, minv: &Wide, g: &Grp, Ghost(db): Ghost<Db>, Ghost(k): Ghost<GK>) -> (r: OutR)
    requires
        tab.ok(),
        maxv@ == max_val(), minv@ == -max_val(),
        rep_db(db),
        g.cnt@ > 0,
        g.cnt@ == cntk(db, k), g.sum@ == sumk(db, k),
        k.name == ov(g.name), k.tag == g.tag@, k.plabel == ov(g.plabel),
    ensures
        ov(r.name) == ov(g.name), r.tag@ == g.tag@, ov(r.plabel) == ov(g.plabel), r.cnt@ == g.cnt@,
        float_within(r.vw@, sumk(db, k), absk(db, k), cntk(db, k) - 1),
        match r.bits {
            Some(b) => f64_val(b) == r.vw@ && f64_in_range(r.vw@),
            None => !f64_in_range(r.vw@),
        },
{
    proof { lemma_pow2_consts(); lemma_abs_le(db, k); }
    let e = &g.sum;
    let inr = !e.lt(minv) && !maxv.lt(e);
    let name = copy_opt(&g.name);
    let tag = copy_bytes(g.tag.as_slice());
    let plabel = copy_opt(&g.plabel);
    let cnt = g.cnt.dup();
    if inr {
        proof { assert(f64_in_range(e@)); }
        let b0 = e.f64_bits();
        let one = Wide::from_i128(1);
        if g.cnt.eq(&one) {
            proof {
                lemma_single_rep(db, k);
                let x = choose|x: u64| f64_val(x) == sumk(db, k);
                lemma_f64_in_range(x);
                assert(f64_finite(x));
            }
            let b = bits_exact(tab, e);
            let vw = wide_of_bits(tab, b);
            proof { lemma_f64_in_range(b); }
            OutR { name, tag, plabel, cnt, vw, bits: Some(b) }
        } else {
            let vw = wide_of_bits(tab, b0);
            proof {
                lemma_rounded_within(b0, e@, absk(db, k), cntk(db, k) - 1);
            }
            OutR { name, tag, plabel, cnt, vw, bits: Some(b0) }
        }
    } else {
        proof { assert(!f64_in_range(e@)); }
        OutR { name, tag, plabel, cnt, vw: e.dup(), bits: None }
    }
}

pub open spec fn rows_ok(db: Db, p: Params, rows: Seq<OutR>) -> bool {
    &&& forall|i: int| 0 <= i < rows.len() ==> row_ok(db, p, rview(#[trigger] rows[i], p.p2))
    &&& forall|i: int| 0 <= i < rows.len() ==> match (#[trigger] rows[i]).bits {
        Some(b) => f64_val(b) == rows[i].vw@ && f64_in_range(rows[i].vw@),
        None => !f64_in_range(rows[i].vw@),
    }
    &&& forall|i: int, j: int| 0 <= i < j < rows.len() ==> !same_group(rview(#[trigger] rows[i], p.p2), rview(#[trigger] rows[j], p.p2))
    &&& forall|g_name: Option<Seq<u8>>, g_tag: Seq<u8>, g_plabel: Option<Seq<u8>>|
        #[trigger] cntk(db, GK { fy: p.p1, stmt: p.p2, uom: p.p3, name: g_name, tag: g_tag, plabel: g_plabel }) > 0 ==>
        exists|i: int| 0 <= i < rows.len() && ov((#[trigger] rows[i]).name) == g_name && rows[i].tag@ == g_tag && ov(rows[i].plabel) == g_plabel
}

pub fn gather(m: &GMap, tab: &Pow2Tab, Ghost(db): Ghost<Db>, p1: i64, p2: &[u8], p3: &[u8]) -> (rows: Vec<OutR>)
    requires state_ok(*m, *tab, db),
    ensures rows_ok(db, params(p1 as int, p2@, p3@), rows@),
{
    let ghost p = params(p1 as int, p2@, p3@);
    let ghost mm = *m;
    proof { lemma_pow2_consts(); }
    let m53 = Wide::from_i128(9007199254740991);
    let maxv = m53.mul(&tab.t[2045]);
    let minv = maxv.neg();
    let ok = okey_of(p1, p2, p3);
    let inner_opt = m.get(&ok);
    let mut rows: Vec<OutR> = Vec::new();
    let inner = match inner_opt {
        None => {
            proof {
                assert(mm.at(ok.view()) is None);
                assert forall|g_name: Option<Seq<u8>>, g_tag: Seq<u8>, g_plabel: Option<Seq<u8>>|
                    #[trigger] cntk(db, GK { fy: p.p1, stmt: p.p2, uom: p.p3, name: g_name, tag: g_tag, plabel: g_plabel }) > 0 implies
                    exists|i: int| 0 <= i < rows@.len() && ov((#[trigger] rows@[i]).name) == g_name && rows@[i].tag@ == g_tag && ov(rows@[i].plabel) == g_plabel by {
                    let k = GK { fy: p.p1, stmt: p.p2, uom: p.p3, name: g_name, tag: g_tag, plabel: g_plabel };
                    assert(gv_c(mm, k) == cntk(db, k));
                    assert(gk_at(mm, k) is None);
                }
            }
            return rows;
        }
        Some(i) => i,
    };
    let ents = inner.rows();
    let ghost iv = arc_val(&inner);
    let ghost entries = iv.entries();
    proof {
        assert(mm.at(ok.view()) == Some(iv));
        inner.map_model();
        verus_prooflib::ordered::lemma_ordered_unique(entries);
        assert(ents@.map_values(|row: (Key, Arc<Grp>)| (row.0.view(), *row.1)) == entries);
    }
    let mut i = 0;
    while i < ents.len()
        invariant
            i <= ents.len(),
            tab.ok(), state_ok(*m, *tab, db), mm == *m,
            p == params(p1 as int, p2@, p3@),
            ok.view() == okey(p1 as int, p2@, p3@),
            mm.at(ok.view()) == Some(iv),
            iv.wf(), entries == iv.entries(),
            ents@.map_values(|row: (Key, Arc<Grp>)| (row.0.view(), *row.1)) == entries,
            maxv@ == max_val(), minv@ == -max_val(),
            forall|a: int, b: int| 0 <= a < b < entries.len() ==> (#[trigger] entries[a]).0 != (#[trigger] entries[b]).0,
            forall|row: (KeyView, Grp)| #![trigger entries.contains(row)] entries.contains(row) ==> iv.at(row.0) == Some(row.1),
            rows.len() == i,
            forall|j: int| 0 <= j < i ==> {
                &&& ov((#[trigger] rows@[j]).name) == ov(arc_val(&ents@[j].1).name)
                &&& rows@[j].tag@ == arc_val(&ents@[j].1).tag@
                &&& ov(rows@[j].plabel) == ov(arc_val(&ents@[j].1).plabel)
            },
            forall|j: int| 0 <= j < i ==> row_ok(db, p, rview(#[trigger] rows@[j], p.p2)),
            forall|j: int| 0 <= j < i ==> match (#[trigger] rows@[j]).bits {
                Some(b) => f64_val(b) == rows@[j].vw@ && f64_in_range(rows@[j].vw@),
                None => !f64_in_range(rows@[j].vw@),
            },
        decreases ents.len() - i,
    {
        let ent = &ents[i];
        let g = &ent.1;
        let ghost gv = arc_val(g);
        let ghost k = GK { fy: p1 as int, stmt: p2@, uom: p3@, name: ov(gv.name), tag: gv.tag@, plabel: ov(gv.plabel) };
        proof {
            assert(entries[i as int] == (ent.0.view(), gv));
            assert(entries.contains(entries[i as int]));
            assert(iv.at(ent.0.view()) == Some(gv));
            // gm_ok: the entry's key is its own fields' key
            assert(ent.0.view() == gv.ik());
            assert(gk_at(mm, k) == Some(gv));
            assert(gv_c(mm, k) == gv.cnt@ && gv_s(mm, k) == gv.sum@);
            assert(gv.cnt@ == cntk(db, k) && gv.sum@ == sumk(db, k));
            assert(gv.cnt@ > 0);
        }
        let r = make_row(tab, &maxv, &minv, g, Ghost(db), Ghost(k));
        proof {
            assert(k_params(k) == p);
            lemma_row_ok(db, p, k, r.vw@);
            assert(rview(r, p.p2) == row_of_group(p, k, r.vw@, cntk(db, k)));
        }
        rows.push(r);
        i += 1;
    }
    proof {
        assert forall|a: int, b: int| 0 <= a < b < rows@.len() implies !same_group(rview(#[trigger] rows@[a], p.p2), rview(#[trigger] rows@[b], p.p2)) by {
            let (ga, gb) = (arc_val(&ents@[a].1), arc_val(&ents@[b].1));
            assert(entries[a].0 != entries[b].0);
            assert(entries[a] == (ents@[a].0.view(), ga) && entries[b] == (ents@[b].0.view(), gb));
            assert(entries.contains(entries[a]) && entries.contains(entries[b]));
            assert(ents@[a].0.view() == ga.ik() && ents@[b].0.view() == gb.ik());
            if same_group(rview(rows@[a], p.p2), rview(rows@[b], p.p2)) {
                assert(ga.ik() == gb.ik());
            }
        }
        assert forall|g_name: Option<Seq<u8>>, g_tag: Seq<u8>, g_plabel: Option<Seq<u8>>|
            #[trigger] cntk(db, GK { fy: p.p1, stmt: p.p2, uom: p.p3, name: g_name, tag: g_tag, plabel: g_plabel }) > 0 implies
            exists|i: int| 0 <= i < rows@.len() && ov((#[trigger] rows@[i]).name) == g_name && rows@[i].tag@ == g_tag && ov(rows@[i].plabel) == g_plabel by {
            let k = GK { fy: p.p1, stmt: p.p2, uom: p.p3, name: g_name, tag: g_tag, plabel: g_plabel };
            assert(gv_c(mm, k) == cntk(db, k));
            assert(gk_at(mm, k) is Some);
            let gg = gk_at(mm, k)->0;
            assert(iv.at(ikey(g_name, g_tag, g_plabel)) == Some(gg));
            assert(iv.view().dom().contains(ikey(g_name, g_tag, g_plabel)));
            assert(entries.contains((ikey(g_name, g_tag, g_plabel), gg)));
            let j = choose|j: int| 0 <= j < entries.len() && entries[j] == (ikey(g_name, g_tag, g_plabel), gg);
            assert(entries[j] == (ents@[j].0.view(), arc_val(&ents@[j].1)));
            assert(gg.ik() == ikey(g_name, g_tag, g_plabel));
            lemma_ikey_inj(ov(gg.name), gg.tag@, ov(gg.plabel), g_name, g_tag, g_plabel);
            assert(arc_val(&ents@[j].1) == gg);
            assert(ents@.len() == entries.len());
            assert(0 <= j < rows@.len());
            assert(ov(rows@[j].name) == g_name && rows@[j].tag@ == g_tag && ov(rows@[j].plabel) == g_plabel);
        }
    }
    rows
}


pub fn cmp_opt(x: &Option<Vec<u8>>, y: &Option<Vec<u8>>) -> (c: i8)
    ensures
        (c < 0) == opt_lt(ov(*x), ov(*y)),
        (c == 0) == (ov(*x) == ov(*y)),
        (c > 0) == opt_lt(ov(*y), ov(*x)),
{
    match (x, y) {
        (None, None) => 0,
        (Some(_), None) => -1,
        (None, Some(_)) => 1,
        (Some(a), Some(b)) => compare_bytes(a.as_slice(), b.as_slice()),
    }
}

/// ORDER BY total_value DESC, name, tag, plabel on two rows.
pub fn lt_row(a: &OutR, b: &OutR, Ghost(p2): Ghost<Seq<u8>>) -> (r: bool)
    ensures r == row_lt(rview(*a, p2), rview(*b, p2)),
{
    if b.vw.lt(&a.vw) { return true; }
    if a.vw.lt(&b.vw) { return false; }
    let c = cmp_opt(&a.name, &b.name);
    if c < 0 { return true; }
    if c > 0 { return false; }
    let t = compare_bytes(a.tag.as_slice(), b.tag.as_slice());
    if t < 0 { return true; }
    if t > 0 { return false; }
    cmp_opt(&a.plabel, &b.plabel) < 0
}


pub type OutT = (Option<Vec<u8>>, Option<Vec<u8>>, Option<Vec<u8>>, Option<Vec<u8>>, Option<u64>, i64);

pub fn cnt_i64(c: &Wide) -> (r: Option<i64>)
    ensures
        r matches Some(v) ==> v as int == c@,
        r is None ==> !(-9223372036854775808 <= c@ <= 9223372036854775807),
{
    match c.to_i128() {
        Some(x) => if x >= -9223372036854775808i128 && x <= 9223372036854775807i128 { Some(x as i64) } else { None },
        None => None,
    }
}

pub proof fn lemma_index_permutation(sorted: Seq<usize>, original: Seq<usize>, length: nat)
    requires verus_common::sort::same_bag(sorted, original), original.len() == length,
        forall|index: int| 0 <= index < original.len() ==> (#[trigger] original[index]) as int == index,
        forall|index: int| 0 <= index < sorted.len() ==> (#[trigger] sorted[index]) < length,
    ensures sorted.no_duplicates(),
        forall|index: usize| index < length ==> #[trigger] sorted.contains(index),
{
    assert forall|left: int, right: int| 0 <= left < right < sorted.len() implies
        (#[trigger] sorted[left]) != (#[trigger] sorted[right]) by {
        if sorted[left] == sorted[right] {
            let target = sorted[left];
            let weight = |item: usize| if item == target { 1int } else { 0int };
            lemma_sum_single_match(original, weight, target as int);
            assert(sum(sorted, weight) == sum(original, weight));
            lemma_sum_remove(sorted, left, weight);
            let smaller = sorted.remove(left);
            assert(smaller[right - 1] == target);
            lemma_sum_ge_term(smaller, weight, right - 1);
        }
    }
    assert forall|index: usize| index < length implies #[trigger] sorted.contains(index) by {
        let weight = |item: usize| if item == index { 1int } else { 0int };
        lemma_sum_single_match(original, weight, index as int);
        assert(sum(sorted, weight) == 1);
        if !sorted.contains(index) {
            assert forall|position: int| 0 <= position < sorted.len() implies weight(#[trigger] sorted[position]) == 0 by {}
            lemma_sum_zero(sorted, weight);
        }
    }
}

pub open spec fn rv_of(rows: Seq<OutR>, p2: Seq<u8>, i: usize) -> OutRow {
    rview(rows[i as int], p2)
}

pub open spec fn full_of(rows: Seq<OutR>, p2: Seq<u8>, sorted: Seq<usize>) -> Seq<OutRow> {
    sorted.map_values(|i: usize| rv_of(rows, p2, i))
}

proof fn lemma_full_ok(db: Db, p: Params, rows: Seq<OutR>, sorted: Seq<usize>)
    requires
        rows_ok(db, p, rows),
        forall|i: int| 0 <= i < sorted.len() ==> (#[trigger] sorted[i]) < rows.len(),
    ensures
        forall|i: int| 0 <= i < full_of(rows, p.p2, sorted).len() ==> row_ok(db, p, #[trigger] full_of(rows, p.p2, sorted)[i]),
{
    let full = full_of(rows, p.p2, sorted);
    assert forall|i: int| 0 <= i < full.len() implies row_ok(db, p, #[trigger] full[i]) by {
        assert(full[i] == rv_of(rows, p.p2, sorted[i]));
    }
}

proof fn lemma_full_order(db: Db, p: Params, rows: Seq<OutR>, sorted: Seq<usize>)
    requires
        rows_ok(db, p, rows),
        sorted.no_duplicates(),
        forall|i: int| 0 <= i < sorted.len() ==> (#[trigger] sorted[i]) < rows.len(),
        forall|i: int, j: int| 0 <= i < j < sorted.len() ==> !row_lt(rv_of(rows, p.p2, sorted[j]), rv_of(rows, p.p2, sorted[i])),
    ensures
        forall|i: int, j: int| 0 <= i < j < full_of(rows, p.p2, sorted).len() ==>
            !same_group(#[trigger] full_of(rows, p.p2, sorted)[i], #[trigger] full_of(rows, p.p2, sorted)[j]) && !row_lt(full_of(rows, p.p2, sorted)[j], full_of(rows, p.p2, sorted)[i]),
{
    let full = full_of(rows, p.p2, sorted);
    assert forall|i: int, j: int| 0 <= i < j < full.len() implies !same_group(#[trigger] full[i], #[trigger] full[j]) && !row_lt(full[j], full[i]) by {
        assert(full[i] == rv_of(rows, p.p2, sorted[i]) && full[j] == rv_of(rows, p.p2, sorted[j]));
        let (a, b) = (sorted[i] as int, sorted[j] as int);
        assert(a != b);
        if a < b { assert(!same_group(rview(rows[a], p.p2), rview(rows[b], p.p2))); }
        else { assert(!same_group(rview(rows[b], p.p2), rview(rows[a], p.p2))); }
    }
}

#[verifier::rlimit(100)]
proof fn lemma_full_complete(db: Db, p: Params, rows: Seq<OutR>, sorted: Seq<usize>)
    requires
        rows_ok(db, p, rows),
        rows.len() <= usize::MAX,
        forall|i: int| 0 <= i < sorted.len() ==> (#[trigger] sorted[i]) < rows.len(),
        forall|index: usize| index < rows.len() ==> #[trigger] sorted.contains(index),
    ensures
        forall|g_name: Option<Seq<u8>>, g_stmt: Option<Seq<u8>>, g_tag: Option<Seq<u8>>, g_plabel: Option<Seq<u8>>|
            #[trigger] group_count(db, p, g_name, g_stmt, g_tag, g_plabel) > 0 && having(db, p, g_name, g_stmt, g_tag, g_plabel) ==>
            exists|i: int| 0 <= i < full_of(rows, p.p2, sorted).len() && (#[trigger] full_of(rows, p.p2, sorted)[i]).name == g_name
                && full_of(rows, p.p2, sorted)[i].stmt == g_stmt && full_of(rows, p.p2, sorted)[i].tag == g_tag && full_of(rows, p.p2, sorted)[i].plabel == g_plabel,
{
    let full = full_of(rows, p.p2, sorted);
    assert forall|g_name: Option<Seq<u8>>, g_stmt: Option<Seq<u8>>, g_tag: Option<Seq<u8>>, g_plabel: Option<Seq<u8>>|
        #[trigger] group_count(db, p, g_name, g_stmt, g_tag, g_plabel) > 0 && having(db, p, g_name, g_stmt, g_tag, g_plabel) implies
        exists|i: int| 0 <= i < full.len() && (#[trigger] full[i]).name == g_name && full[i].stmt == g_stmt && full[i].tag == g_tag && full[i].plabel == g_plabel by {
        lemma_group_nonempty(db, p, g_name, g_stmt, g_tag, g_plabel);
        let gt = g_tag->0;
        let k = GK { fy: p.p1, stmt: p.p2, uom: p.p3, name: g_name, tag: gt, plabel: g_plabel };
        assert(cntk(db, k) > 0);
        let a = choose|a: int| 0 <= a < rows.len() && ov((#[trigger] rows[a]).name) == g_name && rows[a].tag@ == gt && ov(rows[a].plabel) == g_plabel;
        assert((a as usize) as int == a);
        assert(sorted.contains(a as usize));
        let pos = choose|pos: int| 0 <= pos < sorted.len() && sorted[pos] == a as usize;
        assert(full[pos] == rv_of(rows, p.p2, sorted[pos]));
        assert(rv_of(rows, p.p2, a as usize) == rview(rows[a], p.p2));
        assert(0 <= pos < full.len());
    }
}

pub proof fn lemma_full(db: Db, p: Params, rows: Seq<OutR>, sorted: Seq<usize>)
    requires
        rows_ok(db, p, rows),
        rows.len() <= usize::MAX,
        sorted.no_duplicates(),
        forall|i: int| 0 <= i < sorted.len() ==> (#[trigger] sorted[i]) < rows.len(),
        forall|index: usize| index < rows.len() ==> #[trigger] sorted.contains(index),
        forall|i: int, j: int| 0 <= i < j < sorted.len() ==> !row_lt(rv_of(rows, p.p2, sorted[j]), rv_of(rows, p.p2, sorted[i])),
    ensures
        full_result(db, p, full_of(rows, p.p2, sorted)),
{
    lemma_full_ok(db, p, rows, sorted);
    lemma_full_order(db, p, rows, sorted);
    lemma_full_complete(db, p, rows, sorted);
}

pub proof fn lemma_not_fits(db: Db, p: Params, full: Seq<OutRow>, t: int)
    requires
        full_result(db, p, full),
        0 <= t < full.len(), t < 200,
        !out_fits(full[t]),
    ensures !answer_fits(db, p),
{
    let lim = limit_rows(full, 200);
    assert(query_result(db, p, lim));
    if full.len() <= 200 { assert(lim == full); } else { assert(lim == full.take(200)); }
    assert(lim[t] == full[t]);
}

pub proof fn lemma_lim_prefix(full: Seq<OutRow>, m: int)
    requires m == if full.len() < 200 { full.len() as int } else { 200int },
    ensures limit_rows(full, 200) =~= full.take(m),
{
    if full.len() <= 200 { assert(full.take(m) =~= full); }
}

pub fn finish(rows: Vec<OutR>, Ghost(db): Ghost<Db>, p1: i64, p2: &[u8], p3: &[u8]) -> (r: Option<Vec<OutT>>)
    requires rows_ok(db, params(p1 as int, p2@, p3@), rows@),
    ensures
        r matches Some(out) ==> query_result(db, params(p1 as int, p2@, p3@), out_view(out@)),
        r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i]),
        r is None ==> !answer_fits(db, params(p1 as int, p2@, p3@)),
{
    let ghost p = params(p1 as int, p2@, p3@);
    let n = rows.len();
    let mut indices: Vec<usize> = Vec::new();
    let mut i = 0;
    while i < n
        invariant i <= n, indices.len() == i, forall|j: int| 0 <= j < indices.len() ==> (#[trigger] indices@[j]) as int == j,
        decreases n - i,
    {
        indices.push(i);
        i += 1;
    }
    let ghost rs = rows@;
    let ghost valid = |x: usize| x < n;
    let ghost le = |a: usize, b: usize| !row_lt(rv_of(rs, p2@, b), rv_of(rs, p2@, a));
    let rows_ref = &rows;
    let compare = |a: usize, b: usize| -> (r: bool)
        requires a < rows_ref.len(), b < rows_ref.len(),
        ensures r == !row_lt(rv_of(rows_ref@, p2@, b), rv_of(rows_ref@, p2@, a)),
    { !lt_row(&rows_ref[b], &rows_ref[a], Ghost(p2@)) };
    proof {
        assert forall|a: usize, b: usize| valid(a) && valid(b) implies #[trigger] le(a, b) || le(b, a) by {
            assert(crow(rv_of(rs, p2@, a)) && crow(rv_of(rs, p2@, b)));
            lemma_row_lt_order(rv_of(rs, p2@, a), rv_of(rs, p2@, b), rv_of(rs, p2@, a));
        }
        assert forall|a: usize, b: usize, c: usize| valid(a) && valid(b) && valid(c) && #[trigger] le(a, b) && #[trigger] le(b, c) implies le(a, c) by {
            assert(crow(rv_of(rs, p2@, a)) && crow(rv_of(rs, p2@, b)) && crow(rv_of(rs, p2@, c)));
            lemma_row_le_trans(rv_of(rs, p2@, a), rv_of(rs, p2@, b), rv_of(rs, p2@, c));
        }
    }
    let sorted = verus_common::sort::sort_idx(&indices, &compare, Ghost(le), Ghost(valid));
    proof {
        lemma_index_permutation(sorted@, indices@, n as nat);
        verus_prooflib::lemma_sum_constant_one(sorted@);
        verus_prooflib::lemma_sum_constant_one(indices@);
        assert(verus_common::sort::same_bag(sorted@, indices@));
        lemma_full(db, p, rs, sorted@);
        assert(rs.len() <= usize::MAX);
    }
    let ghost full = full_of(rs, p2@, sorted@);
    let ghost lim = limit_rows(full, 200);
    proof { assert(full_result(db, p, full)); assert(query_result(db, p, lim)); }
    let m = if n < 200 { n } else { 200 };
    let mut out: Vec<OutT> = Vec::new();
    let mut t = 0;
    while t < m
        invariant
            t <= m, m <= n, m <= 200, n == rows.len(), sorted@.len() == n,
            rs == rows@, full == full_of(rs, p2@, sorted@), full_result(db, p, full),
            rows_ok(db, p, rs), p == params(p1 as int, p2@, p3@),
            forall|j: int| 0 <= j < sorted@.len() ==> (#[trigger] sorted@[j]) < rows.len(),
            m == if n < 200 { n } else { 200 },
            out.len() == t,
            forall|j: int| 0 <= j < t ==> out_view(out@)[j] == full[j],
            forall|j: int| 0 <= j < t ==> out_fits(full[j]),
        decreases m - t,
    {
        let idx = sorted[t];
        let r = &rows[idx];
        let ghost fr = full[t as int];
        proof { assert(fr == rview(rs[idx as int], p2@)); }
        let c = cnt_i64(&r.cnt);
        let ok = r.bits.is_some() && c.is_some();
        if !ok {
            proof {
                assert(full.len() == n);
                assert(!out_fits(fr)) by {
                    if r.bits.is_none() { assert(!f64_in_range(r.vw@)); }
                    else { assert(c is None); }
                }
                lemma_not_fits(db, p, full, t as int);
            }
            return None;
        }
        let tup: OutT = (copy_opt(&r.name), Some(copy_bytes(p2)), Some(copy_bytes(r.tag.as_slice())), copy_opt(&r.plabel), r.bits, c.unwrap());
        let ghost before = out@;
        out.push(tup);
        proof {
            assert(out_view(out@) =~= out_view(before).push(fr));
            let b = r.bits->0;
            assert(f64_val(b) == r.vw@ && f64_in_range(r.vw@));
            assert(out_fits(fr));
        }
        t += 1;
    }
    proof {
        assert(full.len() == n);
        lemma_lim_prefix(full, m as int);
        assert(out_view(out@) =~= lim);
        assert forall|j: int| 0 <= j < out.len() implies out_fits(#[trigger] out_view(out@)[j]) by {
            assert(out_view(out@)[j] == full[j]);
        }
    }
    Some(out)
}


} // verus!

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


} // verus!

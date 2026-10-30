// Reading the answer: the groups of the asked class with more than 10 rows, rounded, ordered by count.
#![allow(unused_imports)]
#![verifier::loop_isolation(false)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_common::bytes::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, lemma_ordered_unique};
use verus_prooflib::wide::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::esc::*;
use crate::accmap::*;
use crate::gmap::*;
use crate::merge::*;
use crate::wideval::*;
use crate::f64bits::*;
use crate::rank::*;
use crate::emit::*;
use crate::kernel::{state_ok, state_vals};
use crate::engine;
use crate::merge::arc_val;

verus! {

/// The rows are exactly the groups of the class with more than 10 rows, ordered for the answer.
pub open spec fn gv_ok(m: GMap, db: Db) -> bool {
    &&& gm_ok(m)
    &&& state_vals(m, db)
}

pub open spec fn rows_ok(db: Db, p: Params, rows: Seq<OutR>) -> bool {
    &&& full_result(db, p, rows.map_values(|r: OutR| rview(r)))
    &&& forall|i: int| 0 <= i < rows.len() ==> bits_ok(#[trigger] rows[i]) && rows[i].ci as int == rows[i].cnt@
}

/// The row is the answer row of the stored group g.
pub open spec fn from_ent(r: OutR, g: Grp) -> bool {
    &&& ov(r.tag) == ov(g.tag) && ov(r.ver) == ov(g.ver)
    &&& r.cnt@ == g.cnt@ && r.ci as int == g.cnt@
}

pub open spec fn tmp_a(t: Index<OutR>, entries: Seq<(KeyView, Grp)>, i: int, db: Db, p: Params) -> bool {
    forall|key: KeyView| #![trigger t.at(key)] t.at(key) is Some ==> {
        let r = t.at(key)->0;
        &&& key == rk(r)
        &&& row_ok(db, p, rview(r)) && bits_ok(r)
        &&& exists|j: int| 0 <= j < i && entries[j].1.cnt@ > 10 && from_ent(r, #[trigger] entries[j].1)
    }
}

pub open spec fn tmp_b(t: Index<OutR>, entries: Seq<(KeyView, Grp)>, i: int) -> bool {
    forall|j: int| #![trigger entries[j]] 0 <= j < i && entries[j].1.cnt@ > 10 ==>
        exists|r: OutR| t.at(rk(r)) == Some(r) && from_ent(r, entries[j].1)
}

/// The group stored under the class key and a group key is the one that key names.
pub open spec fn entries_ok(entries: Seq<(KeyView, Grp)>) -> bool {
    &&& forall|a: int, b: int| 0 <= a < b < entries.len() ==> (#[trigger] entries[a]).0 != (#[trigger] entries[b]).0
    &&& forall|j: int| 0 <= j < entries.len() ==> (#[trigger] entries[j]).0 == entries[j].1.ik()
}

pub proof fn lemma_distinct_groups(entries: Seq<(KeyView, Grp)>, a: int, b: int)
    requires
        entries_ok(entries), 0 <= a < entries.len(), 0 <= b < entries.len(),
        ov(entries[a].1.tag) == ov(entries[b].1.tag), ov(entries[a].1.ver) == ov(entries[b].1.ver),
    ensures a == b,
{
    assert(entries[a].1.ik() == entries[b].1.ik());
    if a < b { assert(entries[a].0 != entries[b].0); }
    if b < a { assert(entries[b].0 != entries[a].0); }
}

/// The temporary index holds the answer rows of the class, in answer order.
pub open spec fn tmp_ok(db: Db, p: Params, t: Index<OutR>) -> bool {
    &&& t.wf()
    &&& rows_ok(db, p, t.entries().map_values(|e: (KeyView, OutR)| e.1))
}

/// Inserting the row of entry i under its own key keeps both temp-index invariants.
proof fn lemma_insert(tprev: Index<OutR>, t: Index<OutR>, kv: KeyView, r: OutR, entries: Seq<(KeyView, Grp)>, i: int, db: Db, p: Params)
    requires
        entries_ok(entries), 0 <= i < entries.len(),
        tmp_a(tprev, entries, i, db, p), tmp_b(tprev, entries, i),
        forall|q: KeyView| #![trigger t.at(q)] t.at(q) == if q == kv { Some(r) } else { tprev.at(q) },
        kv == rk(r), row_ok(db, p, rview(r)), bits_ok(r),
        entries[i].1.cnt@ > 10, from_ent(r, entries[i].1),
    ensures
        tmp_a(t, entries, i + 1, db, p), tmp_b(t, entries, i + 1),
{
    assert forall|q: KeyView| #![trigger t.at(q)] t.at(q) is Some implies ({
        let rr = t.at(q)->0;
        &&& q == rk(rr)
        &&& row_ok(db, p, rview(rr)) && bits_ok(rr)
        &&& exists|j: int| 0 <= j < i + 1 && entries[j].1.cnt@ > 10 && from_ent(rr, #[trigger] entries[j].1)
    }) by {
        if q != kv {
            let rr = tprev.at(q)->0;
            let j = choose|j: int| 0 <= j < i && entries[j].1.cnt@ > 10 && from_ent(rr, #[trigger] entries[j].1);
            assert(0 <= j < i + 1);
        } else {
            assert(0 <= i < i + 1 && entries[i].1.cnt@ > 10 && from_ent(r, entries[i].1));
        }
    }
    assert forall|j: int| #![trigger entries[j]] 0 <= j < i + 1 && entries[j].1.cnt@ > 10 implies
        exists|rr: OutR| t.at(rk(rr)) == Some(rr) && from_ent(rr, entries[j].1) by {
        if j == i {
            assert(t.at(rk(r)) == Some(r));
        } else {
            let r0 = choose|r0: OutR| tprev.at(rk(r0)) == Some(r0) && from_ent(r0, entries[j].1);
            if rk(r0) == kv {
                lemma_rkey_order(r0.ci, rik(r0), r.ci, rik(r));
                assert(rik(r0).1 == rik(r).1);
                lemma_ikey_inj(ov(r0.tag), ov(r0.ver), ov(r.tag), ov(r.ver));
                lemma_distinct_groups(entries, j, i);
            }
            assert(t.at(rk(r0)) == Some(r0));
        }
    }
}

/// An entry with at most 10 rows adds nothing.
proof fn lemma_skip(t: Index<OutR>, entries: Seq<(KeyView, Grp)>, i: int, db: Db, p: Params)
    requires
        0 <= i < entries.len(), entries[i].1.cnt@ <= 10,
        tmp_a(t, entries, i, db, p), tmp_b(t, entries, i),
    ensures
        tmp_a(t, entries, i + 1, db, p), tmp_b(t, entries, i + 1),
{
    assert forall|q: KeyView| #![trigger t.at(q)] t.at(q) is Some implies ({
        let rr = t.at(q)->0;
        &&& q == rk(rr)
        &&& row_ok(db, p, rview(rr)) && bits_ok(rr)
        &&& exists|j: int| 0 <= j < i + 1 && entries[j].1.cnt@ > 10 && from_ent(rr, #[trigger] entries[j].1)
    }) by {
        let rr = t.at(q)->0;
        let j = choose|j: int| 0 <= j < i && entries[j].1.cnt@ > 10 && from_ent(rr, #[trigger] entries[j].1);
    }
    assert forall|j: int| #![trigger entries[j]] 0 <= j < i + 1 && entries[j].1.cnt@ > 10 implies
        exists|rr: OutR| t.at(rk(rr)) == Some(rr) && from_ent(rr, entries[j].1) by {
        assert(j != i);
    }
}

/// After the last entry the temp index holds exactly the answer rows of the class, in order.
proof fn lemma_finish(db: Db, p: Params, mm: GMap, iv: Index<Grp>, entries: Seq<(KeyView, Grp)>, t: Index<OutR>)
    requires
        gv_ok(mm, db), engine::safe(db, p),
        mm.at(okey(p.p1, p.p2)) == Some(iv), iv.wf(), entries == iv.entries(), entries_ok(entries),
        t.wf(),
        tmp_a(t, entries, entries.len() as int, db, p), tmp_b(t, entries, entries.len() as int),
    ensures
        tmp_ok(db, p, t),
{
    t.model();
    iv.model();
    let es = t.entries();
    let rows = es.map_values(|e: (KeyView, OutR)| e.1);
    let full = rows.map_values(|r: OutR| rview(r));
    assert forall|x: int| #![trigger es[x]] 0 <= x < es.len() implies ({
        let r = es[x].1;
        &&& es[x].0 == rk(r) && row_ok(db, p, rview(r)) && bits_ok(r) && r.ci as int == r.cnt@
        &&& exists|j: int| 0 <= j < entries.len() && entries[j].1.cnt@ > 10 && from_ent(r, #[trigger] entries[j].1)
    }) by {
        assert(t.at(es[x].0) == Some(es[x].1));
        let r = es[x].1;
        let j = choose|j: int| 0 <= j < entries.len() && entries[j].1.cnt@ > 10 && from_ent(r, #[trigger] entries[j].1);
    }
    assert forall|x: int| 0 <= x < rows.len() implies bits_ok(#[trigger] rows[x]) && rows[x].ci as int == rows[x].cnt@ by {
        assert(rows[x] == es[x].1);
    }
    assert forall|x: int| 0 <= x < full.len() implies row_ok(db, p, #[trigger] full[x]) by {
        assert(full[x] == rview(es[x].1));
    }
    assert forall|a: int, b: int| 0 <= a < b < full.len() implies !same_group(#[trigger] full[a], #[trigger] full[b]) && !row_lt(full[b], full[a]) by {
        let (ra, rb) = (es[a].1, es[b].1);
        assert(full[a] == rview(ra) && full[b] == rview(rb));
        assert(key_lt(es[a].0, es[b].0));
        lemma_rkey_order(ra.ci, rik(ra), rb.ci, rik(rb));
        if same_group(full[a], full[b]) {
            let ja = choose|j: int| 0 <= j < entries.len() && entries[j].1.cnt@ > 10 && from_ent(ra, #[trigger] entries[j].1);
            let jb = choose|j: int| 0 <= j < entries.len() && entries[j].1.cnt@ > 10 && from_ent(rb, #[trigger] entries[j].1);
            lemma_distinct_groups(entries, ja, jb);
            assert(ra.ci == rb.ci);
            assert(rik(ra) == rik(rb));
            assert(rk(ra) == rk(rb));
            verus_prooflib::ordered::lemma_key_order(es[a].0, es[a].0);
        }
    }
    assert forall|g_tag: Option<Seq<u8>>, g_ver: Option<Seq<u8>>| group_count(db, p, g_tag, g_ver) > 0 && having(db, p, g_tag, g_ver) implies
        exists|x: int| 0 <= x < full.len() && (#[trigger] full[x]).tag == g_tag && full[x].version == g_ver by {
        lemma_aggs(db, p, g_tag, g_ver);
        let k = k_of(p, g_tag, g_ver);
        assert(gv_c(mm, k) == cntk(db, k));
        let g = gk_at(mm, k)->0;
        assert(gk_at(mm, k) is Some);
        assert(iv.at(ikey(g_tag, g_ver)) == Some(g));
        let j = choose|j: int| 0 <= j < entries.len() && entries[j].0 == ikey(g_tag, g_ver);
        assert(entries[j].1 == g);
        assert(g.cnt@ > 10);
        assert(entries[j].0 == g.ik());
        lemma_ikey_inj(ov(g.tag), ov(g.ver), g_tag, g_ver);
        let r = choose|r: OutR| t.at(rk(r)) == Some(r) && from_ent(r, entries[j].1);
        let x = choose|x: int| 0 <= x < es.len() && es[x].0 == rk(r);
        assert(es[x].1 == r);
        assert(full[x] == rview(r));
    }
}

/// Every stored group of the class passes the engine's check: the query is safe.
proof fn lemma_all_safe(db: Db, p: Params, mm: GMap, iv: Index<Grp>, entries: Seq<(KeyView, Grp)>)
    requires
        gv_ok(mm, db),
        mm.at(okey(p.p1, p.p2)) == Some(iv), iv.wf(), entries == iv.entries(),
        forall|j: int| 0 <= j < entries.len() ==> engine::grp_ok(#[trigger] entries[j].1.abs@),
    ensures
        engine::safe(db, p),
{
    engine::lemma_safe_iff(db, p);
    assert forall|k: GK| #![trigger cntk(db, k)] k.y == p.p1 && k.uom == p.p2 && cntk(db, k) > 0 implies engine::grp_ok(absk(db, k)) by {
        assert(gv_c(mm, k) == cntk(db, k));
        assert(gk_at(mm, k) is Some);
        let g = gk_at(mm, k)->0;
        assert(okey(k.y, k.uom) == okey(p.p1, p.p2));
        assert(iv.at(ikey(k.tag, k.ver)) == Some(g));
        iv.model();
        let j = choose|j: int| 0 <= j < entries.len() && entries[j].0 == ikey(k.tag, k.ver);
        assert(entries[j].1 == g);
        assert(gv_a(mm, k) == absk(db, k));
    }
}

#[verifier::rlimit(1000)]
#[verifier::spinoff_prover]
pub fn gather(m: &GMap, tab: &Pow2Tab, Ghost(db): Ghost<Db>, p1: i64, p2: &[u8]) -> (t: Option<Index<OutR>>)
    requires state_ok(*m, *tab, db),
    ensures
        t matches Some(ix) ==> tmp_ok(db, params(p1 as int, p2@), ix),
        envelope(db) && engine::safe(db, params(p1 as int, p2@)) ==> t is Some,
{
    let ghost p = params(p1 as int, p2@);
    let ghost mm = *m;
    proof { lemma_pow2_consts(); }
    let m53 = Wide::from_i128(9007199254740991);
    let maxv = m53.mul(&tab.t[2045]);
    let minv = maxv.neg();
    let eleven = Wide::from_i128(11);
    let ok = okey_of(p1, p2);
    let mut t: Index<OutR> = Index::new();
    let inner_opt = m.get(&ok);
    let inner = match inner_opt {
        None => {
            proof {
                assert(mm.at(ok.view()) is None);
                assert forall|g_tag: Option<Seq<u8>>, g_ver: Option<Seq<u8>>| group_count(db, p, g_tag, g_ver) > 0 && having(db, p, g_tag, g_ver) implies false by {
                    lemma_aggs(db, p, g_tag, g_ver);
                    let k = k_of(p, g_tag, g_ver);
                    assert(gv_c(mm, k) == cntk(db, k));
                    assert(gk_at(mm, k) is None);
                }
                engine::lemma_safe_iff(db, p);
                assert forall|k: GK| #![trigger cntk(db, k)] k.y == p.p1 && k.uom == p.p2 && cntk(db, k) > 0 implies engine::grp_ok(absk(db, k)) by {
                    assert(gv_c(mm, k) == cntk(db, k));
                    assert(okey(k.y, k.uom) == okey(p.p1, p.p2));
                    assert(gk_at(mm, k) is None);
                }
                let es = t.entries();
                assert(es.len() == 0);
                assert(es.map_values(|e: (KeyView, OutR)| e.1) =~= Seq::<OutR>::empty());
                assert(es.map_values(|e: (KeyView, OutR)| e.1).map_values(|r: OutR| rview(r)) =~= Seq::<OutRow>::empty());
            }
            return Some(t);
        }
        Some(i) => i,
    };
    let ents = inner.rows();
    let ghost iv = arc_val(&inner);
    let ghost entries = iv.entries();
    proof {
        assert(mm.at(ok.view()) == Some(iv));
        inner.map_model();
        lemma_ordered_unique(entries);
        assert(ents@.map_values(|row: (Key, Arc<Grp>)| (row.0.view(), *row.1)) == entries);
        assert(state_ok(mm, *tab, db));
        iv.model();
        assert forall|j: int| 0 <= j < entries.len() implies (#[trigger] entries[j]).0 == entries[j].1.ik() by {
            assert(iv.at(entries[j].0) == Some(entries[j].1));
            assert(entries[j].0 == iv.at(entries[j].0)->0.ik());
        }
    }
    let mut i = 0;
    while i < ents.len()
        invariant
            i <= ents.len(),
            tab.ok(), state_ok(mm, *tab, db), mm == *m,
            maxv@ == max_val(), minv@ == -max_val(), eleven@ == 11,
            ok.view() == okey(p1 as int, p2@),
            mm.at(ok.view()) == Some(iv),
            iv.wf(), entries == iv.entries(),
            ents@.map_values(|row: (Key, Arc<Grp>)| (row.0.view(), *row.1)) == entries,
            entries_ok(entries),
            forall|row: (KeyView, Grp)| #![trigger entries.contains(row)] entries.contains(row) ==> iv.at(row.0) == Some(row.1),
            t.wf(),
            tmp_a(t, entries, i as int, db, p),
            tmp_b(t, entries, i as int),
            forall|j: int| 0 <= j < i ==> engine::grp_ok(#[trigger] entries[j].1.abs@),
        decreases ents.len() - i,
    {
        let ent = &ents[i];
        let g = &ent.1;
        let ghost gv = arc_val(g);
        let ghost k = GK { y: p1 as int, uom: p2@, tag: ov(gv.tag), ver: ov(gv.ver) };
        proof {
            assert(entries[i as int] == (ent.0.view(), gv));
            assert(entries.contains(entries[i as int]));
            assert(iv.at(ent.0.view()) == Some(gv));
            assert(ent.0.view() == gv.ik());
            assert(gk_at(mm, k) == Some(gv));
            assert(gv_c(mm, k) == gv.cnt@ && gv_s(mm, k) == gv.sum@ && gv_a(mm, k) == gv.abs@);
            assert(gv.cnt@ == cntk(db, k) && gv.sum@ == sumk(db, k));
        }
        if !engine::abs_ok(&maxv, &g.abs) {
            proof {
                assert(gv.cnt@ > 0);
                if envelope(db) && engine::safe(db, p) {
                    engine::lemma_safe_iff(db, p);
                    assert(k.y == p.p1 && k.uom == p.p2 && cntk(db, k) > 0);
                    assert(engine::grp_ok(absk(db, k)));
                    assert(gv.abs@ == absk(db, k));
                    assert(false);
                }
            }
            return None;
        }
        if !g.cnt.lt(&eleven) {
            proof { lemma_cnt_facts(db, k); }
            let ci = match g.cnt.to_i128() {
                Some(c) => c,
                None => {
                    proof {
                        if envelope(db) { assert(db.num.len() <= max_rows()); assert(false); }
                    }
                    return None;
                }
            };
            let row = make_row(tab, &maxv, &minv, g, ci, Ghost(db), Ghost(k));
            let ghost r = row;
            let ik = ikey_of(&g.tag, &g.ver);
            let key = rank_key(ci, &ik);
            let ghost tprev = t;
            let ghost kv = key.view();
            proof {
                assert(k == k_of(p, ov(gv.tag), ov(gv.ver)));
                lemma_row_ok(db, p, ov(gv.tag), ov(gv.ver), row.vw@);
                assert(rview(row) == OutRow { tag: ov(gv.tag), version: ov(gv.ver), cnt: cntk(db, k), total: Some(row.vw@) });
                assert(kv == rk(row));
                assert(from_ent(row, gv));
            }
            t.put(key, Arc::new(row));
            proof { lemma_insert(tprev, t, kv, r, entries, i as int, db, p); }
        } else {
            proof { lemma_skip(t, entries, i as int, db, p); }
        }
        i += 1;
    }
    proof {
        assert(gv_ok(mm, db));
        lemma_all_safe(db, p, mm, iv, entries);
        lemma_finish(db, p, mm, iv, entries, t);
    }
    Some(t)
}

} // verus!
